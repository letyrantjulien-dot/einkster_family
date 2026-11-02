// E!nkster 397 bench: the board under the sketch, faked on a computer, so that the sketch itself runs here.
// What is faked: the paper (it remembers what it shows, and checks that every partial refresh starts from it),
// the dial, the power chip, the clock chip, the room sensor, the flash files, deep sleep. No network.
#pragma once
#include "host_base.h"
#include "qrcodegen.h"
#include <dirent.h>

// ---- the board's state, kept between wakes by the driver (host.cpp)
struct HostHw {
  bool chipValid;           // the clock chip holds a time
  bool sysValid;            // the processor's clock is set (survives deep sleep, not a power cut)
  uint64_t bootUs;          // when the processor last got power
  int batMv, batPct; bool usb, chg; uint8_t chgState;   // v0.3.0: 1 charging, 2 topping up, 3 full, 4 not charging
  float roomC, roomRH; bool roomThere;
  int wakeCause;            // 0 power on, 2 dial, 4 timer
  int wakePin;
  bool off;                 // switched off by the power chip
  bool slept; uint64_t sleepUs;
  int fullRefreshes, partRefreshes, badPartials, pushes;
  bool pwrLong;             // PWR held long during this wake
  bool sceneEdit;           // a scene changed the state by hand: the next push may not start from the paper
  uint8_t paper[48000];     // what the paper shows
  struct Ev { uint32_t at, hold; int pin; } ev[16]; int nEv;
  // v0.2.0: the typewriter's card and keyboard
  bool sdThere;             // a microSD in the slot
  bool kbThere, kbConnected; int kbBat;
  struct Key { uint32_t at; uint8_t mod, usage; bool up, done; } keys[400]; int nKeys;
  int kbReports;
  int windows, windowRows;  // window refreshes, and how many panel rows the last one had
  int cutAtKey;             // the power goes as this key is typed (0: never)
};
extern HostHw hw;
extern std::string hostFs;               // this device's flash folder
extern std::map<std::string, std::string> hostPrefs;

// ---- time: the sketch calls time(); here it is the bench's clock
static inline time_t hostTime(time_t *t) {
  time_t v = hw.sysValid ? (time_t)(hostUs / 1000000) : (time_t)((hostUs - hw.bootUs) / 1000000);
  if (t) { *t = v; }
  return v;
}
#define time(p) hostTime(p)
static inline uint32_t nowS() { return (uint32_t)time(NULL); }
static inline bool timeValid() { return time(NULL) > 1735689600; }

// ---- settings
class Preferences {
public:
  bool begin(const char *, bool = false) { return true; }
  void end() {}
  int getInt(const char *k, int d = 0) { return hostPrefs.count(k) ? atoi(hostPrefs[k].c_str()) : d; }
  void putInt(const char *k, int v) { hostPrefs[k] = std::to_string(v); }
  uint32_t getUInt(const char *k, uint32_t d = 0) { return hostPrefs.count(k) ? strtoul(hostPrefs[k].c_str(), 0, 10) : d; }
  void putUInt(const char *k, uint32_t v) { hostPrefs[k] = std::to_string(v); }
  bool getBool(const char *k, bool d = false) { return hostPrefs.count(k) ? hostPrefs[k] == "1" : d; }
  void putBool(const char *k, bool v) { hostPrefs[k] = v ? "1" : "0"; }
  String getString(const char *k, const String &d = String()) { return hostPrefs.count(k) ? String(hostPrefs[k].c_str()) : d; }
  size_t getString(const char *k, char *o, size_t n) { if (!n) return 0; std::string v = hostPrefs.count(k) ? hostPrefs[k] : ""; snprintf(o, n, "%s", v.c_str()); return strlen(o); }
  void putString(const char *k, const String &v) { hostPrefs[k] = v.c_str(); }
  void putString(const char *k, const char *v) { hostPrefs[k] = v; }
  bool isKey(const char *k) { return hostPrefs.count(k) > 0; }
  bool remove(const char *k) { return hostPrefs.erase(k) > 0; }
  uint8_t getUChar(const char *k, uint8_t d = 0) { return hostPrefs.count(k) ? atoi(hostPrefs[k].c_str()) : d; }
  void putUChar(const char *k, uint8_t v) { hostPrefs[k] = std::to_string(v); }
  size_t getBytesLength(const char *k) { return hostPrefs.count(k) ? hostPrefs[k].size() / 2 : 0; }
  size_t getBytes(const char *k, void *o, size_t n) { std::string v = hostPrefs.count(k) ? hostPrefs[k] : ""; size_t m = std::min(n, v.size() / 2); for (size_t i = 0; i < m; i++) ((uint8_t *)o)[i] = strtoul(v.substr(i * 2, 2).c_str(), 0, 16); return m; }
  size_t putBytes(const char *k, const void *d, size_t n) { std::string v; char b[4]; for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02x", ((const uint8_t *)d)[i]); v += b; } hostPrefs[k] = v; return n; }
};

// ---- QR codes: Espressif's esp_qrcode is built on Project Nayuki's generator; this is the same generator
typedef const uint8_t *esp_qrcode_handle_t;
enum { ESP_QRCODE_ECC_LOW, ESP_QRCODE_ECC_MED, ESP_QRCODE_ECC_QUART, ESP_QRCODE_ECC_HIGH };
typedef struct { void (*display_func)(esp_qrcode_handle_t); int max_qrcode_version; int qrcode_ecc_level; } esp_qrcode_config_t;
static inline int esp_qrcode_get_size(esp_qrcode_handle_t q) { return qrcodegen_getSize(q); }
static inline bool esp_qrcode_get_module(esp_qrcode_handle_t q, int x, int y) { return qrcodegen_getModule(q, x, y); }
static inline esp_err_t esp_qrcode_generate(esp_qrcode_config_t *cfg, const char *text) {
  static uint8_t buf[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
  if (!qrcodegen_encodeText(text, tmp, buf, (enum qrcodegen_Ecc)cfg->qrcode_ecc_level, qrcodegen_VERSION_MIN, cfg->max_qrcode_version, qrcodegen_Mask_AUTO, true)) return -1;
  cfg->display_func(buf); return ESP_OK;
}

// ---- the board
#define PIN_UP_H 4
#define PIN_PRESS_H 5
#define PIN_DOWN_H 6
static inline void boardInit(bool) {}
static inline bool btn(int pin) {
  uint32_t ms = (uint32_t)((hostUs - hostWakeUs) / 1000);
  for (int i = 0; i < hw.nEv; i++) if (hw.ev[i].pin == pin && ms >= hw.ev[i].at && ms < hw.ev[i].at + hw.ev[i].hold) return true;
  return false;
}
static inline int batMv() { return hw.batMv; }
static inline int batPct() { return hw.batMv ? hw.batPct : -1; }
static inline bool charging() { return hw.chg; }
static inline bool usbIn() { return hw.usb; }
static inline uint8_t chgState() { return hw.usb ? hw.chgState : 0; }
static inline bool pwrLongPressed() { bool l = hw.pwrLong; hw.pwrLong = false; return l; }
static inline void pmuOff() { hw.off = true; }
static inline void rtcChipToSystem() { if (hw.chipValid) hw.sysValid = true; }
static inline void systemToRtcChip() { hw.chipValid = true; }
static inline bool readRoom(float &t, float &h) { if (!hw.roomThere) return false; t = hw.roomC; h = hw.roomRH; return true; }
void epdPush(bool full, const uint8_t *old);
void epdPushRows(int r0, int r1, const uint8_t *old);
static inline bool pwrShortPressed() { return false; }
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(int, int) { return (QueueHandle_t)1; }
[[noreturn]] void sleepFor(uint64_t us);
static inline int wakePin() { return hw.wakeCause == 2 ? hw.wakePin : -1; }
static inline bool coldStart() { return hw.wakeCause == 0; }
static inline const char *resetText() { return hw.wakeCause == 0 ? "power on" : "deep sleep"; }
static inline String devName() { return String("397-BE7C"); }
static inline uint64_t upUs() { return hostUs - hw.bootUs; }
size_t fsSize(const char *p);
void fsAppend(const char *p, const char *d, size_t n);
void fsRotate(const char *p, const char *old);
size_t fsRead(const char *p, uint8_t **out);
