// ek_hid.h: the Cl!cker's Bluetooth keyboard (Bluetooth Low Energy, HID over GATT).
// 154 firmware, from v0.25.2. Not a shared file yet: the pebble may take it later, through the contract.
//
// What it does: the board shows itself as a keyboard called "E!nkster Cl!cker", pairs with "just works"
// (no code to type), remembers every device it has paired with, and talks to one of them at a time:
// the chosen one. A remembered device that is not the chosen one is let in, recognised once its link is
// encrypted, and sent away. In "new" mode only a device pairing for the first time is let in, and it becomes the chosen one.
//
// It sends key presses (a key with its modifiers, then the release) and one consumer key (voice command).
// It keeps no names, and never logs an address: the caller only gets counts and states.
//
// Built on the ESP32 Arduino core's BLE library (Bluedroid), ESP32 Arduino core 3.2.0, by Espressif (Apache 2.0),
// itself from Neil Kolban's ESP32 BLE library. The report map follows the USB HID usage tables.
// Written by Julien and Claude the AI
#pragma once
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLE2902.h>
#include "esp_gap_ble_api.h"

enum { EKH_OFF, EKH_WAITING, EKH_LINKED, EKH_READY };   // off / advertising / connected, not yet encrypted / ready to type

// key codes (USB HID usage page 7) and the one consumer usage
#define EKH_RIGHT 0x4F
#define EKH_LEFT  0x50
#define EKH_DOWN  0x51
#define EKH_UP    0x52
#define EKH_PGUP  0x4B
#define EKH_PGDN  0x4E
#define EKH_ENTER 0x28
#define EKH_CTRL  0x01            // left Ctrl, as a modifier bit
#define EKH_VOICE 0x00CF          // consumer page: voice command (Android: voice assist)

static const uint8_t EKH_MAP[] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,                    // keyboard, report 1
  0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,        //   8 modifier bits
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                //   reserved byte
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,                    //   six keys
  0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
  0xC0,
  0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,                    // consumer control, report 2
  0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,        //   one 16-bit usage
  0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
  0xC0,
};

static BLEServer *ekhSrv = NULL;
static BLEHIDDevice *ekhHid = NULL;
static BLECharacteristic *ekhKb = NULL, *ekhCc = NULL;
static volatile bool ekhConn = false, ekhAuth = false, ekhAdvWanted = false, ekhAdvOn = false;
static volatile bool ekhWrong = false, ekhNewPeer = false, ekhKeysSeen = false;   // ekhKeysSeen: keys exchanged on this link = a new pairing
static volatile uint16_t ekhConnId = 0;
static uint8_t ekhTarget[6]; static bool ekhAny = true;          // ekhAny: "new" mode, anyone may pair
static uint8_t ekhPeer[6];                                       // identity of the device that just authenticated
static volatile int ekhRefused = 0;

static bool ekhSame(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }

struct EkhSrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *p) override {
    ekhConnId = p->connect.conn_id; ekhConn = true; ekhAuth = false; ekhAdvOn = false; ekhKeysSeen = false;
    esp_ble_set_encryption(p->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);   // ask for the encrypted link at once
  }
  void onDisconnect(BLEServer *) override { ekhConn = false; ekhAuth = false; ekhAdvOn = false; }
};

static void ekhGap(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p) {
  if (ev == ESP_GAP_BLE_KEY_EVT) { ekhKeysSeen = true; return; }   // keys are only handed over when a device pairs for the first time
  if (ev != ESP_GAP_BLE_AUTH_CMPL_EVT) return;
  if (!p->ble_security.auth_cmpl.success) return;                // the caller sees "linked" stay, then a time-out
  memcpy(ekhPeer, p->ble_security.auth_cmpl.bd_addr, 6);
  if (ekhAny && ekhKeysSeen) { memcpy(ekhTarget, ekhPeer, 6); ekhAny = false; ekhNewPeer = true; ekhAuth = true; }
  else if (ekhAny) ekhWrong = true;                              // "+ new": a device paired before is sent away, so a new one can pair
  else if (ekhSame(ekhPeer, ekhTarget)) ekhAuth = true;
  else ekhWrong = true;                                          // a remembered device, not the chosen one: sent away in ekhPoll()
}

// Once per wake. Returns false if Bluetooth did not start.
static bool ekhBegin(const char *name, uint8_t batteryPct) {
  if (ekhSrv) return true;
  BLEDevice::init(name);
  if (!BLEDevice::getInitialized()) return false;
  BLEDevice::setCustomGapHandler(ekhGap);
  ekhSrv = BLEDevice::createServer();
  ekhSrv->setCallbacks(new EkhSrvCb());
  ekhHid = new BLEHIDDevice(ekhSrv);
  ekhKb = ekhHid->inputReport(1);
  ekhCc = ekhHid->inputReport(2);
  ekhHid->manufacturer()->setValue("InvisibleChaos");
  ekhHid->pnp(0x02, 0x303A, 0x8154, 0x0100);                     // USB vendor id of Espressif; product and version are ours
  ekhHid->hidInfo(0x00, 0x01);
  ekhHid->reportMap((uint8_t *)EKH_MAP, sizeof EKH_MAP);
  ekhHid->startServices();
  ekhHid->setBatteryLevel(batteryPct);
  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);                                    // keyboard
  adv->addServiceUUID(ekhHid->hidService()->getUUID());
  adv->setScanResponse(true);
  return true;
}

// The chosen device (NULL: "new", anyone may pair). Sends away whoever is linked if it no longer fits.
static void ekhChoose(const uint8_t *addr) {
  if (addr) { memcpy(ekhTarget, addr, 6); ekhAny = false; } else ekhAny = true;
  if (ekhSrv && ekhConn) ekhSrv->disconnect(ekhConnId);
  ekhAuth = false;
}
static void ekhAdvertise(bool on) {
  ekhAdvWanted = on;
  if (!ekhSrv) return;
  if (on && !ekhConn && !ekhAdvOn) { BLEDevice::startAdvertising(); ekhAdvOn = true; }
  if (!on && ekhAdvOn) { BLEDevice::stopAdvertising(); ekhAdvOn = false; }
}
// Call often from the loop: sends away a wrong device, and advertises again after a link ends.
static void ekhPoll() {
  if (!ekhSrv) return;
  if (ekhWrong) { ekhWrong = false; ekhRefused++; if (ekhConn) ekhSrv->disconnect(ekhConnId); }
  if (ekhConn && ekhAuth && ekhKb) {                             // a remembered host may not write its notification switch again
    BLE2902 *d = (BLE2902 *)ekhKb->getDescriptorByUUID((uint16_t)0x2902); if (d && !d->getNotifications()) d->setNotifications(true);
    d = (BLE2902 *)ekhCc->getDescriptorByUUID((uint16_t)0x2902); if (d && !d->getNotifications()) d->setNotifications(true);
  }
  if (ekhAdvWanted && !ekhConn && !ekhAdvOn) { BLEDevice::startAdvertising(); ekhAdvOn = true; }
}
static int ekhState() {
  if (!ekhSrv) return EKH_OFF;
  if (ekhConn && ekhAuth) return EKH_READY;
  if (ekhConn) return EKH_LINKED;
  return ekhAdvWanted ? EKH_WAITING : EKH_OFF;
}
// The device that just paired in "new" mode, once: true and its address.
static bool ekhTakeNew(uint8_t *addr) { if (!ekhNewPeer) return false; ekhNewPeer = false; memcpy(addr, ekhTarget, 6); return true; }

static bool ekhKey(uint8_t mod, uint8_t key) {
  if (ekhState() != EKH_READY) return false;
  uint8_t r[8] = { mod, 0, key, 0, 0, 0, 0, 0 };
  ekhKb->setValue(r, sizeof r); ekhKb->notify();
  delay(12);
  memset(r, 0, sizeof r);
  ekhKb->setValue(r, sizeof r); ekhKb->notify();
  return true;
}
static bool ekhConsumer(uint16_t usage) {
  if (ekhState() != EKH_READY) return false;
  uint8_t r[2] = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
  ekhCc->setValue(r, 2); ekhCc->notify();
  delay(12);
  r[0] = r[1] = 0;
  ekhCc->setValue(r, 2); ekhCc->notify();
  return true;
}
// Every device paired so far, in the stack's order. Returns how many (up to max).
static int ekhBonds(uint8_t (*out)[6], int max) {
  if (!ekhSrv) return 0;
  int n = esp_ble_get_bond_device_num(); if (n <= 0) return 0;
  esp_ble_bond_dev_t *l = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n); if (!l) return 0;
  esp_ble_get_bond_device_list(&n, l);
  if (n > max) n = max;
  for (int i = 0; i < n; i++) memcpy(out[i], l[i].bd_addr, 6);
  free(l);
  return n;
}
// End the link and stop advertising. Bluetooth itself stays up until the next deep sleep.
static void ekhStop() {
  ekhAdvertise(false);
  if (ekhSrv && ekhConn) ekhSrv->disconnect(ekhConnId);
  ekhAuth = false;
}
