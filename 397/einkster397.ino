// EINKSTER 397 · v0.4.0: the keyboard everywhere. (v0.3.0: the archives and the battery log. v0.2.0: the typewriter. v0.1.0: first light.)
// Keyboard (v0.4.0): once paired, it drives every screen next to the dial; Esc always does what a long press does. It cannot
// wake the 397: after a dial or PWR wake the 397 listens for it a minute in the background, and a key brings it back. The E!nkster family on Waveshare's ESP32-S3-ePaper-3.97 (800 x 480, read upright).
// No touch screen: the dial on the left edge does everything.
//   dial up / dial down   previous / next tile, or move in a list, or turn a page
//   press                 open, choose, reveal an answer
//   long press (0.7 s)    back one level; on a tile, back to the Clock
//   PWR, held 1.5 s       switch off cleanly ("OFF" on the paper). Held 4 s, the power chip cuts the power anyway.
//   PWR, short            switches it on when off.
// Typewriter (v0.2.0): a Bluetooth keyboard, the page turned to 800 x 480; Enter saves the line to /typed/<date>.txt on
// the microSD. Saved lines are never edited. Long press on the dial: leave. Device > Typewriter: turn the page.
// Archives (v0.3.0): Kiwix archives turned into text on a computer (shelf_convert.py), read from /shelf on the microSD;
// a title is typed on the keyboard, or picked by its first letter with the dial. Every article shows its source and licence.
// Battery (v0.3.0): a 30-minute log as on the 154 (/log.bin), a page under Device, a CSV on the set-up page.
// Tiles: Clock, Climate, Editions, Typewriter, Archives, Catalog, Device. Editions holds the nine packs the 154 carries, from the same source.
// Device > Set up: a hotspot and a page for the Wi-Fi networks, the clock from the phone, the language and the field log.
// Sleeps 45 s after the last turn of the dial. On the Clock tile it wakes every minute to move the hands, without
// flashing (a clean full refresh once an hour). Every 30 minutes: battery and room climate go into the field log.
// Every hour, with a known network: the time from the internet and the weather from Open-Meteo.
// Shared with the family: ek_shared.h (settings names, journal lines, Wi-Fi list), fonts.h, the built-in packs.
// Board: Waveshare ESP32-S3-ePaper-3.97 (ESP32-S3, 16 MB flash, 8 MB PSRAM, PCF85063, SHTC3, AXP2101-type power chip).
// InvisibleChaos. Written by Julien and Claude the AI.

#define EK_VERSION   "v0.4.0"
#define EK_BOARD_ID  "397"
#define EK_ROLE      "classic"

#ifdef EK_HOST
#include "host.h"                      // the bench: the same code, run on a computer (tests/host)
#else
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "driver/rtc_io.h"
#include "qrcode.h"
#include <SD_MMC.h>                      // v0.2.0: the typewriter's card
#include <BLEDevice.h>                   // v0.2.0: the keyboard (the core's BLE library, on Bluedroid)
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLESecurity.h>
#include <vector>
#include <sys/time.h>
#include <time.h>
#define XPOWERS_CHIP_AXP2101                // the chip XPowersPMU stands for
#include "src/XPowersLib/XPowersLib.h"   // Lewis He's XPowersLib (MIT), the copy Waveshare ships with the board's demo
#endif
#if __has_include("secrets.h")
#include "secrets.h"                     // written by prepare397.py from config.h or the environment
#endif
#include "ek_shared.h"
#include "fonts.h"

#ifndef EK_WIFI_SSID
#define EK_WIFI_SSID ""
#endif
#ifndef EK_WIFI_PASS
#define EK_WIFI_PASS ""
#endif
#ifndef EK_CARD
#define EK_CARD ""
#endif

// ---------------------------------------------------------------- pins (Waveshare's demo code, checked 26.10.2026)
#define PIN_SDA        41
#define PIN_SCL        42
#define PIN_EPD_SCK    11
#define PIN_EPD_MOSI   12
#define PIN_EPD_CS     10
#define PIN_EPD_DC     9
#define PIN_EPD_RST    46
#define PIN_EPD_BUSY   3
#define PIN_UP         4               // the dial, active low
#define PIN_PRESS      5
#define PIN_DOWN       6
#define ADDR_RTC       0x51            // PCF85063
#define ADDR_SHTC3     0x70
#define ADDR_PMU       0x34            // AXP2101 (sold as TG28)

// ---------------------------------------------------------------- the paper: 480 x 800, upright, dial on the left
static int W = 480, H = 800;           // v0.2.0: the typewriter turns the page to 800 x 480
static uint8_t rot = 0;                // 0 upright, 1 landscape, 2 landscape the other way round
#define NW 800                         // the panel's own rows and columns
#define NH 480
#define FB_BYTES (NW / 8 * NH)         // 48000
static uint8_t fb[FB_BYTES];           // 1 = white
static uint8_t *prevFb = NULL;         // what the paper shows now (PSRAM)
static bool epdHot = false;            // v0.2.0: the controller stays awake between the typewriter's window refreshes

// ---------------------------------------------------------------- what survives deep sleep
enum { SCR_TILE, SCR_LIST, SCR_CARD, SCR_QR, SCR_SETUP, SCR_DOC, SCR_TYPE, SCR_KB, SCR_FIND, SCR_READ, SCR_BAT };   // v0.3.0: the archives, the battery
enum { T_CLOCK, T_CLIMATE, T_EDITIONS, T_TYPE, T_SHELF, T_CATALOG, T_DEVICE, N_TILES };
// v0.2.0: the typewriter's types, here because the Arduino build writes function prototypes above the first function
struct TwRep { uint8_t d[24]; uint8_t n; uint16_t handle; uint32_t ms; };
struct KbFound { uint8_t a[6]; uint8_t type; int rssi; char name[24]; uint16_t app; };
struct KeyDe { uint8_t u; const char *n, *s, *a; };
// v0.3.0: the archives and the 30-minute log
struct ShArcInfo { String dir, title, lang, source, licence, date; int format; };
struct ShLine { uint32_t at, next; String key, title, target; int file; uint32_t off, len; };
struct BatSample {                             // 16 bytes, the 154's layout (v2)
  uint32_t t; uint16_t mv; uint8_t wifi, asks;  // asks: on the 397, the battery percentage (255 none)
  int16_t tin10; uint8_t rh; int8_t tout; uint16_t p10, spare;   // spare: the LOGF_ flags
};
static void (*keySink)(uint8_t, uint8_t) = NULL;   // who gets the keys: the typewriter (NULL) or the archives
enum { L_SHELF, L_SECS, L_CARDS, L_CATALOG, L_DEVICE, L_ARCH };
#define RTC_MAGIC 0x39710003                 // v0.3.0: the state grew (the archives, the charge state)
struct RtcState {
  uint32_t magic;
  uint8_t scr, tile, list;
  int16_t cur, top;                    // list cursor and first row shown
  char slug[32];                       // the open pack
  int16_t sec;                         // -1 all cards
  int16_t card, page; uint8_t revealed;
  int16_t qr;                          // catalog row shown as a code
  int16_t doc;                         // 0 about
  // the face, as last drawn (redrawn from these values to make the next partial refresh)
  int32_t faceMin;                     // minutes since 1970 shown by the clock
  int16_t roomT10, roomH;              // room, tenths of a degree; -999 none
  int16_t outT10; int8_t outCode;      // outside now
  int16_t dMax10[3], dMin10[3]; int8_t dCode[3];
  uint32_t wxAt;                       // when the weather came, 0 never
  int8_t batPct; uint8_t charging, usb; uint16_t batMv;
  uint8_t fr;                          // interface language when drawn
  uint8_t nets;                        // networks known when drawn
  uint8_t wxOk, clockOk;               // the weather fresh, the clock set, when drawn
  // housekeeping
  uint16_t partials;                   // partial refreshes since the last full one
  uint32_t lastSample, lastFetch, lastFull, ntpAt;
  uint8_t fetchFails;
  uint32_t run;
  uint32_t wakes;
  // v0.2.0: the typewriter
  char tline[480];                     // the line being typed (kept through sleep, not through a power cut)
  uint8_t caps, dead;                  // Caps Lock; a dead key waiting (1 ^, 2 acute, 3 grave)
  int8_t kbBat;                        // the keyboard's battery, -1 not known
  // v0.3.0: the archives
  uint8_t shCard, shN;                 // a card seen (255 not looked yet), archives on it
  char shArc[48], shAT[40];            // the open archive: folder, title
  char shQ[64], shKey[64];             // what is typed, and its search key
  uint32_t shLb, shAt;                 // titles.txt: the first match, the first row shown
  int16_t shCur; uint8_t shFocus;      // the row chosen; without keyboard 0 the letter, 1 the list
  uint16_t shFile; uint32_t shOff, shLen; int16_t shPage;   // the article open, its page
  // v0.3.0: the battery
  uint8_t chg;                         // 0 none, 1 charging, 2 topping up, 3 full, 4 USB not charging
  uint8_t wifiN, kbOn;                 // since the last sample: Wi-Fi visits, the keyboard connected
};
RTC_DATA_ATTR static RtcState R;
RTC_DATA_ATTR static char flRing[3000];
RTC_DATA_ATTR static uint16_t flLen;
RTC_DATA_ATTR static uint32_t flMagic;
RTC_DATA_ATTR static uint64_t flRunStartUs;

static Preferences prefs;
static bool uiFr = false;
#define L(en, fr) (uiFr ? (fr) : (en))
static String tzCache;

// ================================================================ drawing
static inline void px(int x, int y, bool ink) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  int X, Y;
  if (rot == 0) { X = y; Y = NH - 1 - x; }            // upright: the panel turned a quarter (Waveshare's "270")
  else if (rot == 1) { X = x; Y = y; }                // landscape, as the panel is wired (Waveshare's "0")
  else { X = NW - 1 - x; Y = NH - 1 - y; }            // landscape, the other way round
  uint8_t &b = fb[(X >> 3) + Y * (NW / 8)], m = 0x80 >> (X & 7);
  if (ink) b &= ~m; else b |= m;
}
static void clearFb() { memset(fb, 0xFF, FB_BYTES); }
static bool inkAt(const uint8_t *buf, int x, int y) {   // for the bench: a pixel of a picture, as the page is turned now
  int X, Y; if (rot == 0) { X = y; Y = NH - 1 - x; } else if (rot == 1) { X = x; Y = y; } else { X = NW - 1 - x; Y = NH - 1 - y; }
  return !(buf[(X >> 3) + Y * (NW / 8)] & (0x80 >> (X & 7)));
}
static void rect(int x, int y, int w, int h, bool ink) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, ink); }
static void frame(int x, int y, int w, int h, int t) { rect(x, y, w, t, true); rect(x, y + h - t, w, t, true); rect(x, y, t, h, true); rect(x + w - t, y, t, h, true); }
static void disc(int cx, int cy, int r, bool ink = true) { for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) if (x * x + y * y <= r * r) px(cx + x, cy + y, ink); }
static void ring(int cx, int cy, int r, int t) { for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) { int d = x * x + y * y; if (d <= r * r && d > (r - t) * (r - t)) px(cx + x, cy + y, true); } }
static void invert(int x, int y, int w, int h) {
  for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) {
    if (i < 0 || j < 0 || i >= W || j >= H) continue;
    int X, Y;
    if (rot == 0) { X = j; Y = NH - 1 - i; } else if (rot == 1) { X = i; Y = j; } else { X = NW - 1 - i; Y = NH - 1 - j; }
    fb[(X >> 3) + Y * (NW / 8)] ^= 0x80 >> (X & 7);
  }
}

// ---- text (DejaVu Sans Bold, fonts.h, shared with the 154)
static int decodeUtf8(const char *c, uint16_t *out, int max) {
  int n = 0; const uint8_t *p = (const uint8_t *)c;
  while (*p && n < max - 1) {
    uint32_t cp;
    if (*p < 0x80) cp = *p++;
    else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = 0xFFFD; p += 4; }
    else { p++; while ((*p & 0xC0) == 0x80) p++; cp = 0xFFFD; }
    if (cp == '\t') cp = ' ';
    else if (cp < 32 && cp != '\n') continue;
    out[n++] = (uint16_t)cp;
  }
  return n;
}
static int glyphW(const EkFont &f, uint16_t cp, int s = 1) {
  if (cp == '\n') return 0;
  int i = ek_glyph_index(cp); return (i < 0 ? f.height / 2 + 3 : f.glyphs[i].width) * s;
}
static void glyph(const EkFont &f, int x, int y, uint16_t cp, bool ink, int s = 1) {
  int i = ek_glyph_index(cp);
  if (i < 0) {                          // a character the fonts do not have: a hollow box, never a crash
    if (cp == ' ' || cp == '\n') return;
    int w = glyphW(f, cp, s);
    rect(x + s, y + 3 * s, w - 3 * s, s, ink); rect(x + s, y + (f.height - 5) * s, w - 3 * s, s, ink);
    rect(x + s, y + 3 * s, s, (f.height - 7) * s, ink); rect(x + w - 3 * s, y + 3 * s, s, (f.height - 7) * s, ink);
    return;
  }
  const EkGlyph &g = f.glyphs[i]; int bpr = (g.width + 7) / 8;
  const uint8_t *b = f.bits + g.offset;
  for (int r = 0; r < f.height; r++) for (int c = 0; c < g.width; c++)
    if (b[r * bpr + (c >> 3)] & (0x80 >> (c & 7))) { if (s == 1) px(x + c, y + r, ink); else rect(x + c * s, y + r * s, s, s, ink); }
}
static uint16_t tbuf[1600];
static int textWidth(const EkFont &f, const String &s, int sc = 1) {
  int n = decodeUtf8(s.c_str(), tbuf, 400), w = 0;
  for (int i = 0; i < n; i++) w += glyphW(f, tbuf[i], sc);
  return w;
}
// one line from x; cut with an ellipsis past maxW; returns the width drawn
static int textLeft(const EkFont &f, int x, int y, const String &s, bool ink = true, int maxW = 9999, int sc = 1) {
  int n = decodeUtf8(s.c_str(), tbuf, 400), w = 0, full = 0;
  for (int i = 0; i < n; i++) full += glyphW(f, tbuf[i], sc);
  int ell = full > maxW ? glyphW(f, 0x2026, sc) : 0;
  for (int i = 0; i < n; i++) {
    int gw = glyphW(f, tbuf[i], sc);
    if (full > maxW && w + gw > maxW - ell) { glyph(f, x + w, y, 0x2026, ink, sc); w += ell; break; }
    glyph(f, x + w, y, tbuf[i], ink, sc); w += gw;
  }
  return w;
}
static void textAt(const EkFont &f, int cx, int y, const String &s, int sc = 1) {
  int w = textWidth(f, s, sc); if (w > W - 16) w = W - 16;
  textLeft(f, cx - w / 2, y, s, true, W - 16, sc);
}
static void textRight(const EkFont &f, int xr, int y, const String &s) { textLeft(f, xr - textWidth(f, s), y, s); }

// word wrap: line starts/ends into the arrays; returns the number of lines
static int wrap(const EkFont &f, const uint16_t *t, int n, int w, int *st, int *en, int maxLines) {
  int lines = 0, i = 0;
  while (i < n && lines < maxLines) {
    while (i < n && t[i] == ' ') i++;
    if (i >= n) break;
    int s = i, lastSpace = -1, x = 0;
    while (i < n && t[i] != '\n') {
      int gw = glyphW(f, t[i]);
      if (x + gw > w) break;
      if (t[i] == ' ' || t[i] == '-' || t[i] == '/') lastSpace = i;
      x += gw; i++;
    }
    int e = i;
    if (i < n && t[i] != '\n' && lastSpace > s) { e = (t[lastSpace] == ' ') ? lastSpace : lastSpace + 1; i = lastSpace + 1; }
    if (i < n && t[i] == '\n') i++;
    st[lines] = s; en[lines] = e; lines++;
  }
  return lines;
}
// a paragraph block; returns the height used
static int para(const EkFont &f, int x, int y, int w, const String &s, int maxLines = 60, bool centered = false) {
  static int st[80], en[80];
  int n = decodeUtf8(s.c_str(), tbuf, 1600);
  int lines = wrap(f, tbuf, n, w, st, en, maxLines > 80 ? 80 : maxLines);
  for (int l = 0; l < lines; l++) {
    int lw = 0; for (int i = st[l]; i < en[l]; i++) lw += glyphW(f, tbuf[i]);
    int xx = centered ? x + (w - lw) / 2 : x;
    for (int i = st[l]; i < en[l]; i++) { glyph(f, xx, y + l * f.height, tbuf[i], true); xx += glyphW(f, tbuf[i]); }
  }
  return lines * f.height;
}

// ---- seven-segment digits for the clock
static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
static void digit(int x, int y, int d, int w, int h, int k) {
  uint8_t m = SEG[d]; int hh = h / 2;
  if (m & 0x01) rect(x + k / 2, y, w - k, k, true);
  if (m & 0x02) rect(x + w - k, y + k / 2, k, hh - k / 2, true);
  if (m & 0x04) rect(x + w - k, y + hh, k, hh - k / 2, true);
  if (m & 0x08) rect(x + k / 2, y + h - k, w - k, k, true);
  if (m & 0x10) rect(x, y + hh, k, hh - k / 2, true);
  if (m & 0x20) rect(x, y + k / 2, k, hh - k / 2, true);
  if (m & 0x40) rect(x + k / 2, y + hh - k / 2, w - k, k, true);
}

// ---- weather marks, drawn at any size (s = 1 is the 154's size)
static void cloud(int cx, int cy, int s) {
  disc(cx - 13 * s, cy + 3 * s, 10 * s); disc(cx + s, cy - 5 * s, 14 * s); disc(cx + 14 * s, cy + 4 * s, 10 * s);
  rect(cx - 13 * s, cy + 3 * s, 27 * s, 11 * s, true);
}
static void sun(int cx, int cy, int r, int s) {
  disc(cx, cy, r);
  for (int a = 0; a < 8; a++) {
    float ang = a * PI / 4;
    for (int k = r + 4 * s; k <= r + 9 * s; k++) rect(cx + (int)(cosf(ang) * k) - s, cy + (int)(sinf(ang) * k) - s, 2 * s + 1, 2 * s + 1, true);
  }
}
static void wxMark(int code, int cx, int cy, int s) {
  if (code < 0) return;                                          // no forecast: no mark, the words say it
  if (code == 0) { sun(cx, cy, 13 * s, s); return; }
  if (code <= 2) { sun(cx - 9 * s, cy - 9 * s, 9 * s, s); rect(cx - 30 * s, cy - 4 * s, 60 * s, 30 * s, false); cloud(cx + 4 * s, cy + 8 * s, s); return; }
  if (code == 45 || code == 48) { for (int i = 0; i < 4; i++) rect(cx + (-26 + (i % 2) * 6) * s, cy + (-14 + i * 9) * s, 46 * s, 4 * s, true); return; }
  cloud(cx, cy - 6 * s, s);
  if (code == 3) return;
  bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  if (code >= 95) { for (int j = 0; j < 9 * s; j++) rect(cx + 2 * s - j / 2, cy + 12 * s + j, 4 * s, 1, true); rect(cx - 3 * s, cy + 20 * s, 8 * s, 3 * s, true); for (int j = 0; j < 9 * s; j++) rect(cx - s - j / 2, cy + 23 * s + j, 4 * s, 1, true); return; }
  for (int i = -1; i <= 1; i++) {
    int x = cx + i * 12 * s;
    if (snow) { disc(x, cy + 17 * s, 3 * s); disc(x + 5 * s, cy + 27 * s, 3 * s); }
    else for (int j = 0; j < 12 * s; j++) rect(x - j / 3, cy + 14 * s + j, 2 * s, 1, true);
  }
}
static const char *codeWord(int c) {
  if (c < 0) return L("no forecast yet", "pas de pr\xC3\xA9visions");
  if (c == 0) return L("clear", "d\xC3\xA9gag\xC3\xA9");
  if (c <= 2) return L("partly cloudy", "\xC3\xA9" "claircies");
  if (c == 3) return L("overcast", "couvert");
  if (c == 45 || c == 48) return L("fog", "brouillard");
  if (c <= 57) return L("drizzle", "bruine");
  if (c <= 67) return L("rain", "pluie");
  if (c <= 77) return L("snow", "neige");
  if (c <= 82) return L("showers", "averses");
  if (c <= 86) return L("snow showers", "averses de neige");
  return L("thunderstorm", "orage");
}

// ================================================================ the board (the bench replaces this part)
#ifndef EK_HOST
static XPowersPMU pmu;
static bool pmuOk = false;
static SPIClass epdSpi(FSPI);
static uint8_t bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static uint8_t tobcd(int v) { return ((v / 10) << 4) | (v % 10); }
static bool i2cRead(uint8_t a, uint8_t reg, uint8_t *b, int n) {
  Wire.beginTransmission(a); Wire.write(reg); if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)a, n) != n) return false;
  for (int i = 0; i < n; i++) b[i] = Wire.read();
  return true;
}
static bool i2cWrite(uint8_t a, uint8_t reg, const uint8_t *b, int n) {
  Wire.beginTransmission(a); Wire.write(reg); for (int i = 0; i < n; i++) Wire.write(b[i]); return Wire.endTransmission() == 0;
}
static uint32_t nowS() { return (uint32_t)time(NULL); }
static bool timeValid() { return time(NULL) > 1735689600; }       // after 1.1.2025
static void boardInit(bool cold) {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  pmuOk = pmu.begin(Wire, ADDR_PMU, PIN_SDA, PIN_SCL);
  if (pmuOk) {
    pmu.setALDO1Voltage(3300); pmu.setALDO2Voltage(3300); pmu.setALDO3Voltage(3300);
    pmu.enableALDO1(); pmu.enableALDO2(); pmu.enableALDO3();      // ALDO3 feeds the paper (Waveshare's EPD_Power_ON)
    if (cold) {                                                   // the same settings as Waveshare's own firmware
      pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
      pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
      pmu.setSysPowerDownVoltage(2600);
      pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
      pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_1S);
      pmu.enableBattDetection(); pmu.enableVbusVoltageMeasure(); pmu.enableBattVoltageMeasure(); pmu.enableSystemVoltageMeasure();
      pmu.enableTemperatureMeasure();
      pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
      pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
      pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA);
      pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
      pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
      pmu.setButtonBatteryChargeVoltage(3000); pmu.enableButtonBatteryCharge();
      pmu.setLowBatWarnThreshold(10); pmu.setLowBatShutdownThreshold(5);
      pmu.fuelGaugeControl(true, true);
    }
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ); pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_LONG_IRQ | XPOWERS_AXP2101_PKEY_SHORT_IRQ);   // v0.2.0: short = pair
  }
  for (int p : { PIN_UP, PIN_PRESS, PIN_DOWN }) rtc_gpio_deinit((gpio_num_t)p);   // back from the sleep's RTC function
  pinMode(PIN_UP, INPUT_PULLUP); pinMode(PIN_PRESS, INPUT_PULLUP); pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(PIN_EPD_CS, OUTPUT); digitalWrite(PIN_EPD_CS, HIGH);
  pinMode(PIN_EPD_DC, OUTPUT); pinMode(PIN_EPD_RST, OUTPUT); digitalWrite(PIN_EPD_RST, HIGH);
  pinMode(PIN_EPD_BUSY, INPUT);
  epdSpi.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, -1);
}
static bool btn(int pin) { return digitalRead(pin) == LOW; }
static int batMv() { return pmuOk && pmu.isBatteryConnect() ? pmu.getBattVoltage() : 0; }
static int batPct() { return pmuOk && pmu.isBatteryConnect() ? pmu.getBatteryPercent() : -1; }
static bool charging() { return pmuOk && pmu.isCharging(); }
static bool usbIn() { return pmuOk && pmu.isVbusIn(); }
static uint8_t chgState() {                    // v0.3.0: what the charger is doing
  if (!pmuOk || !pmu.isVbusIn()) return 0;
  switch (pmu.getChargerStatus()) {
    case XPOWERS_AXP2101_CHG_TRI_STATE: case XPOWERS_AXP2101_CHG_PRE_STATE: case XPOWERS_AXP2101_CHG_CC_STATE: return 1;
    case XPOWERS_AXP2101_CHG_CV_STATE: return 2;
    case XPOWERS_AXP2101_CHG_DONE_STATE: return 3;
    default: return 4;
  }
}
static bool pwrShortFlag = false, pwrLongFlag = false;
static void pwrRead() { if (!pmuOk) return; pmu.getIrqStatus(); if (pmu.isPekeyLongPressIrq()) pwrLongFlag = true; if (pmu.isPekeyShortPressIrq()) pwrShortFlag = true; pmu.clearIrqStatus(); }
static bool pwrLongPressed() { pwrRead(); bool l = pwrLongFlag; pwrLongFlag = false; return l; }
static bool pwrShortPressed() { pwrRead(); bool s = pwrShortFlag; pwrShortFlag = false; return s; }
static void pmuOff() { if (pmuOk) pmu.shutdown(); }
// ---- PCF85063: keeps world time (UTC) through sleep and power-off
static void rtcChipToSystem() {
  uint8_t r[7];
  if (!i2cRead(ADDR_RTC, 0x04, r, 7)) return;
  if (r[0] & 0x80) return;                                       // the oscillator stopped: not a valid time
  struct tm t = {};
  t.tm_sec = bcd(r[0] & 0x7F); t.tm_min = bcd(r[1] & 0x7F); t.tm_hour = bcd(r[2] & 0x3F);
  t.tm_mday = bcd(r[3] & 0x3F); t.tm_mon = bcd(r[5] & 0x1F) - 1; t.tm_year = bcd(r[6]) + 100;
  setenv("TZ", "UTC0", 1); tzset();
  time_t e = mktime(&t);
  setenv("TZ", tzCache.c_str(), 1); tzset();
  if (e > 1735689600) { struct timeval tv = { e, 0 }; settimeofday(&tv, NULL); }
}
static void systemToRtcChip() {
  time_t e = time(NULL); struct tm t; gmtime_r(&e, &t);
  uint8_t c = 0x00; i2cWrite(ADDR_RTC, 0x00, &c, 1);
  uint8_t r[7] = { tobcd(t.tm_sec), tobcd(t.tm_min), tobcd(t.tm_hour), tobcd(t.tm_mday), (uint8_t)t.tm_wday, tobcd(t.tm_mon + 1), tobcd(t.tm_year - 100) };
  i2cWrite(ADDR_RTC, 0x04, r, 7);
}
// ---- SHTC3: one measurement, then back to sleep
static bool shtcCmd(uint16_t c) { Wire.beginTransmission(ADDR_SHTC3); Wire.write(c >> 8); Wire.write(c & 0xFF); return Wire.endTransmission() == 0; }
static bool readRoom(float &t, float &h) {
  if (!shtcCmd(0x3517)) return false;                            // wake
  delayMicroseconds(300);
  if (!shtcCmd(0x7866)) return false;                            // measure, temperature first, normal mode
  delay(15);
  uint8_t b[6]; if (Wire.requestFrom(ADDR_SHTC3, 6) != 6) return false;
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  shtcCmd(0xB098);                                               // sleep
  t = -45 + 175 * ((b[0] << 8) | b[1]) / 65536.0f; h = 100 * ((b[3] << 8) | b[4]) / 65536.0f;
  return t > -40 && t < 85;
}
// ---- the paper (SSD1677-type controller; command sequence from Waveshare's EPD_3in97 driver, MIT)
static void epdCmd(uint8_t c) { epdSpi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0)); digitalWrite(PIN_EPD_DC, LOW); digitalWrite(PIN_EPD_CS, LOW); epdSpi.transfer(c); digitalWrite(PIN_EPD_CS, HIGH); epdSpi.endTransaction(); }
static void epdData(uint8_t d) { epdSpi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0)); digitalWrite(PIN_EPD_DC, HIGH); digitalWrite(PIN_EPD_CS, LOW); epdSpi.transfer(d); digitalWrite(PIN_EPD_CS, HIGH); epdSpi.endTransaction(); }
static void epdBuf(const uint8_t *b, size_t n) {
  static uint8_t chunk[2400];                                    // through internal RAM, in pieces
  epdSpi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0)); digitalWrite(PIN_EPD_DC, HIGH); digitalWrite(PIN_EPD_CS, LOW);
  for (size_t i = 0; i < n; i += sizeof chunk) { size_t k = min(sizeof chunk, n - i); memcpy(chunk, b + i, k); epdSpi.writeBytes(chunk, k); }
  digitalWrite(PIN_EPD_CS, HIGH); epdSpi.endTransaction();
}
static void epdWait() { delay(5); uint32_t t = millis(); while (digitalRead(PIN_EPD_BUSY) == HIGH && millis() - t < 15000) delay(5); }
static void epdInit(bool part) {
  digitalWrite(PIN_EPD_RST, HIGH); delay(10); digitalWrite(PIN_EPD_RST, LOW); delay(2); digitalWrite(PIN_EPD_RST, HIGH); delay(10);
  epdWait(); epdCmd(0x12); epdWait();                           // software reset (the picture memory is kept)
  epdCmd(0x18); epdData(0x80);                                   // the controller's own thermometer
  epdCmd(0x0C); epdData(0xAE); epdData(0xC7); epdData(0xC3); epdData(0xC0); epdData(0x80);
  epdCmd(0x01); epdData((NH - 1) % 256); epdData((NH - 1) / 256); epdData(0x02);
  epdCmd(0x3C); epdData(part ? 0x80 : 0x01);                     // border
  epdCmd(0x11); epdData(0x01);                                   // x up, y down
  epdCmd(0x44); epdData(0); epdData(0); epdData((NW - 1) % 256); epdData((NW - 1) / 256);
  epdCmd(0x45); epdData((NH - 1) % 256); epdData((NH - 1) / 256); epdData(0); epdData(0);
  epdCmd(0x4E); epdData(0); epdData(0);
  epdCmd(0x4F); epdData(0); epdData(0);
  epdWait();
}
// full: the clean refresh (it flashes). Partial: only what changed moves, from 'old' to fb.
static void epdPush(bool full, const uint8_t *old) {
  epdInit(!full);
  epdCmd(0x24); epdBuf(fb, FB_BYTES);
  epdCmd(0x4E); epdData(0); epdData(0); epdCmd(0x4F); epdData(0); epdData(0);
  epdCmd(0x26); epdBuf(full ? fb : old, FB_BYTES);
  epdCmd(0x22); epdData(full ? 0xF7 : 0xFF); epdCmd(0x20); epdWait();
  epdCmd(0x10); epdData(0x01);                                   // the controller sleeps; the picture stays
  epdHot = false;
}
// v0.2.0: refresh only panel rows r0..r1 (the typewriter's changed rows): both pictures for that window, then the
// partial waveform. The controller stays awake between them, so there is no reset each time.
static void epdPushRows(int r0, int r1, const uint8_t *old) {
  if (!epdHot) { epdInit(true); epdHot = true; }
  int a0 = NH - 1 - r0, a1 = NH - 1 - r1, n = (r1 - r0 + 1) * (NW / 8);
  epdCmd(0x44); epdData(0); epdData(0); epdData((NW - 1) % 256); epdData((NW - 1) / 256);
  epdCmd(0x45); epdData(a0 % 256); epdData(a0 / 256); epdData(a1 % 256); epdData(a1 / 256);
  epdCmd(0x4E); epdData(0); epdData(0); epdCmd(0x4F); epdData(a0 % 256); epdData(a0 / 256);
  epdCmd(0x24); epdBuf(fb + r0 * (NW / 8), n);
  epdCmd(0x4E); epdData(0); epdData(0); epdCmd(0x4F); epdData(a0 % 256); epdData(a0 / 256);
  epdCmd(0x26); epdBuf(old + r0 * (NW / 8), n);
  epdCmd(0x22); epdData(0xFF); epdCmd(0x20); epdWait();
}
static void sleepFor(uint64_t us) {
  if (epdHot) { epdCmd(0x10); epdData(0x01); epdHot = false; }   // v0.2.0: the controller sleeps too
  uint64_t mask = (1ULL << PIN_UP) | (1ULL << PIN_PRESS) | (1ULL << PIN_DOWN);
  esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // keeps the dial's pull-ups while asleep
  for (int p : { PIN_UP, PIN_PRESS, PIN_DOWN }) { rtc_gpio_pullup_en((gpio_num_t)p); rtc_gpio_pulldown_dis((gpio_num_t)p); }
  if (us) esp_sleep_enable_timer_wakeup(us);
  esp_deep_sleep_start();
}
static int wakePin() {                    // which part of the dial woke it, or -1
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return -1;
  uint64_t m = esp_sleep_get_ext1_wakeup_status();
  if (m & (1ULL << PIN_UP)) return PIN_UP;
  if (m & (1ULL << PIN_DOWN)) return PIN_DOWN;
  if (m & (1ULL << PIN_PRESS)) return PIN_PRESS;
  return -1;
}
static bool wokeByTimer() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER; }
static bool coldStart() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED; }
static const char *resetText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on"; case ESP_RST_SW: return "restart"; case ESP_RST_PANIC: return "crash";
    case ESP_RST_BROWNOUT: return "brownout"; case ESP_RST_DEEPSLEEP: return "deep sleep"; case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_USB: return "usb"; default: return "other";
  }
}
static String devName() { uint64_t mac = ESP.getEfuseMac(); char d[24]; snprintf(d, sizeof d, "397-%04X", (unsigned)((mac >> 32) & 0xFFFF)); return String(d); }
// ---- files
static bool fsUp = false;
static void fsMount() { if (!fsUp) fsUp = LittleFS.begin(true); }
static size_t fsSize(const char *p) { fsMount(); File f = LittleFS.open(p, "r"); size_t n = f ? f.size() : 0; if (f) f.close(); return n; }
static void fsAppend(const char *p, const char *d, size_t n) { fsMount(); File f = LittleFS.open(p, "a"); if (f) { f.write((const uint8_t *)d, n); f.close(); } }
static void fsRotate(const char *p, const char *old) { fsMount(); LittleFS.remove(old); LittleFS.rename(p, old); }
static size_t fsRead(const char *p, uint8_t **out) {   // v0.3.0: a whole file into PSRAM (the caller frees)
  fsMount(); *out = NULL; File f = LittleFS.open(p, "r"); if (!f) return 0;
  size_t n = f.size(); *out = (uint8_t *)heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM); if (!*out) { f.close(); return 0; }
  n = f.read(*out, n); f.close(); return n;
}
extern "C" uint64_t esp_rtc_get_time_us(void);
static uint64_t upUs() { return esp_rtc_get_time_us(); }     // keeps counting through deep sleep
#endif

// ================================================================ field log
// The journal lines of the family (ek_shared.h). Lines wait in the RTC ring and go to flash in batches, so a power cut
// loses at most the last few. Never a key, a network name, a password or a place.
#define FL_MAGIC 0x464C3937
#define FL_FLUSH_AT 2000
static void flFlush() {
  if (flMagic != FL_MAGIC || !flLen) return;
  if (fsSize("/field.txt") > 40000) fsRotate("/field.txt", "/field.old");
  fsAppend("/field.txt", flRing, flLen); flLen = 0;
}
static void flRaw(const char *s) {
  if (flMagic != FL_MAGIC) { flMagic = FL_MAGIC; flLen = 0; }
  size_t n = strlen(s);
  if (flLen + n + 1 >= sizeof flRing) flFlush();
  if (n + 1 >= sizeof flRing) return;
  memcpy(flRing + flLen, s, n); flRing[flLen + n] = '\n'; flLen += n + 1;
}
static void flEmit(const char *line, void *) { flRaw(line); }
static void FL(const char *type, const String &detail) {
  char line[EK_JOURNAL_MAX + 2]; uint64_t us = upUs() + flRunStartUs;
  struct tm lt; time_t e = time(NULL); bool ok = e > 1735689600; if (ok) localtime_r(&e, &lt);
  ekJournalLine(line, sizeof line, (uint32_t)(us / 100000), ok ? &lt : NULL, 0, type, detail.c_str());
  const char *sec[] = { NULL, NULL, NULL };
  String pw = prefs.getString(EK_SET_HOTSPOT_PW, ""); if (pw.length() >= 4) sec[0] = pw.c_str();
  ekJournalHide(line, sec);
  flRaw(line);
  if (flLen > FL_FLUSH_AT) flFlush();
}
static String mvText(int mv) { return String(mv / 1000.0f, 2) + " V"; }

// ================================================================ state and data
static void readBattery() {
  R.batMv = batMv(); R.batPct = batPct(); R.charging = charging(); R.usb = usbIn(); R.chg = chgState();
}
static void readRoomNow() {
  float t, h;
  if (readRoom(t, h)) { R.roomT10 = (int16_t)lroundf(t * 10) + prefs.getInt(EK_SET_TEMP_OFF, 0); R.roomH = (int16_t)lroundf(h); }
  else { R.roomT10 = -999; R.roomH = -1; }
}
static String deg10(int t10) { if (t10 <= -990) return "--"; char b[12]; snprintf(b, sizeof b, "%.1f\xC2\xB0", t10 / 10.0f); return String(b); }
static String degInt(int t10) { if (t10 <= -990) return "--"; return String((int)lroundf(t10 / 10.0f)) + "\xC2\xB0"; }
static bool wxFresh() { return R.wxAt && nowS() - R.wxAt < 3 * 3600; }
// everything a face shows comes from R, so the same picture can be drawn again after a sleep
static int32_t epochMin();
static void freshen() { R.wxOk = wxFresh(); R.clockOk = timeValid(); if (R.scr == SCR_TILE) R.faceMin = R.clockOk ? epochMin() : 0; }
static EkNets nets;
static EkPrefsKV *kv = NULL;

// ================================================================ built-in packs (the 154's, from the same source)
// packs.h is cut out of einkster154.ino by the build (prepare397.py), so both boards always carry the same text.
#include "packs.h"
struct Builtin { const char *slug, *name, *edition, *cards, *lang, *base; };
static const Builtin BUILTIN[] = {
  { "atlantidia-lite", "Atlantid!a Lite", AT_EDITION, AT_CARDS, "", NULL },
  { "vulcania-lite",   "Vulcan!a Lite",   VU_EDITION, VU_CARDS, "", NULL },
  { "yogia-lite",      "Yog!a Lite",      YO_EDITION, YO_CARDS, "", NULL },
  { "catlas",          "C@tlas",          CA_EDITION, CA_CARDS, "", NULL },
  { "guineapedia-lite", "Guineapedi@ Lite", GU_EDITION, GU_CARDS, "", NULL },
  { "waterfacts-lite",    "W@terfacts! Lite", WF_EDITION,  WF_CARDS,  "en", NULL },
  { "waterfacts-lite-de", "W@terfacts! Lite", WFD_EDITION, WFD_CARDS, "de", "waterfacts-lite" },
  { "waterfacts-lite-fr", "W@terfacts! Lite", WFF_EDITION, WFF_CARDS, "fr", "waterfacts-lite" },
  { "edition-zero",    "Edit!on Zero",    EZ_EDITION, EZ_CARDS, "", NULL },
};
#define N_BUILTIN (int)(sizeof BUILTIN / sizeof BUILTIN[0])
static int builtinIdx(const char *slug) { for (int i = 0; i < N_BUILTIN; i++) if (!strcmp(BUILTIN[i].slug, slug)) return i; return -1; }
// the shelf: one row per family, in the interface language when the family has it
static int shelf[N_BUILTIN], shelfN = 0;
static void buildShelf() {
  shelfN = 0;
  for (int i = 0; i < N_BUILTIN; i++) {
    if (BUILTIN[i].base) continue;
    int pick = i;
    const char *want = uiFr ? "fr" : "en";
    for (int j = 0; j < N_BUILTIN; j++) if (BUILTIN[j].base && !strcmp(BUILTIN[j].base, BUILTIN[i].slug) && !strcmp(BUILTIN[j].lang, want)) pick = j;
    shelf[shelfN++] = pick;
  }
}

// ---- a small JSON reader (the 154's): the whole document in PSRAM, nodes by index
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
struct JNode { uint8_t t; bool b; float n; const char *s; const char *key; int child, next, count; };
static JNode *jdoc = NULL; static int jnN = 0, jnCap = 0;
static char *jarena = NULL; static size_t jaUsed = 0;
static const char *jp;
static void jfree() { if (jdoc) free(jdoc); if (jarena) free(jarena); jdoc = NULL; jarena = NULL; jnN = jnCap = 0; jaUsed = 0; }
static int jnew(uint8_t t) {
  if (jnN == jnCap) {
    int c = jnCap ? jnCap * 2 : 1024;
    JNode *p = (JNode *)heap_caps_realloc(jdoc, c * sizeof(JNode), MALLOC_CAP_SPIRAM);
    if (!p) return -1;
    jdoc = p; jnCap = c;
  }
  JNode &x = jdoc[jnN]; x = {}; x.t = t; x.child = x.next = -1; x.s = ""; x.key = "";
  return jnN++;
}
static void jws() { while (*jp == ' ' || *jp == '\n' || *jp == '\r' || *jp == '\t') jp++; }
static int hex4(const char *h) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    char c = h[i]; v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return -1;
  }
  return v;
}
static char *utf8put(char *w, uint32_t cp) {
  if (cp < 0x80) *w++ = cp;
  else if (cp < 0x800) { *w++ = 0xC0 | (cp >> 6); *w++ = 0x80 | (cp & 63); }
  else if (cp < 0x10000) { *w++ = 0xE0 | (cp >> 12); *w++ = 0x80 | ((cp >> 6) & 63); *w++ = 0x80 | (cp & 63); }
  else { *w++ = 0xF0 | (cp >> 18); *w++ = 0x80 | ((cp >> 12) & 63); *w++ = 0x80 | ((cp >> 6) & 63); *w++ = 0x80 | (cp & 63); }
  return w;
}
static const char *jstring() {
  jp++;
  char *o = jarena + jaUsed, *w = o;
  while (*jp && *jp != '"') {
    char c = *jp++;
    if (c != '\\') { *w++ = c; continue; }
    char e = *jp ? *jp++ : 0;
    if (e == 'n') *w++ = '\n';
    else if (e == 't') *w++ = ' ';
    else if (e == 'r' || e == 'b' || e == 'f') {}
    else if (e == 'u') {
      int u = hex4(jp); if (u < 0) continue; jp += 4;
      uint32_t cp = u;
      if (u >= 0xD800 && u <= 0xDBFF && jp[0] == '\\' && jp[1] == 'u') {
        int l = hex4(jp + 2);
        if (l >= 0xDC00 && l <= 0xDFFF) { cp = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00); jp += 6; }
      }
      w = utf8put(w, cp);
    } else if (e) *w++ = e;
  }
  if (*jp == '"') jp++;
  *w++ = 0;
  jaUsed = w - jarena;
  return o;
}
static int jvalue() {
  jws();
  char c = *jp;
  if (c == '{' || c == '[') {
    bool obj = c == '{'; jp++;
    int me = jnew(obj ? J_OBJ : J_ARR); if (me < 0) return -1;
    int last = -1;
    for (;;) {
      jws();
      if (*jp == (obj ? '}' : ']')) { jp++; break; }
      const char *key = "";
      if (obj) {
        if (*jp != '"') return -1;
        key = jstring(); jws();
        if (*jp != ':') return -1;
        jp++;
      }
      int ch = jvalue(); if (ch < 0) return -1;
      jdoc[ch].key = key;
      if (last < 0) jdoc[me].child = ch; else jdoc[last].next = ch;
      last = ch; jdoc[me].count++;
      jws();
      if (*jp == ',') { jp++; continue; }
      if (*jp == (obj ? '}' : ']')) { jp++; break; }
      return -1;
    }
    return me;
  }
  if (c == '"') { int me = jnew(J_STR); if (me < 0) return -1; const char *s = jstring(); jdoc[me].s = s; return me; }
  if (!strncmp(jp, "true", 4))  { jp += 4; int me = jnew(J_BOOL); if (me >= 0) jdoc[me].b = true; return me; }
  if (!strncmp(jp, "false", 5)) { jp += 5; return jnew(J_BOOL); }
  if (!strncmp(jp, "null", 4))  { jp += 4; return jnew(J_NULL); }
  char *end; float v = strtof(jp, &end);
  if (end == jp) return -1;
  jp = end; int me = jnew(J_NUM); if (me >= 0) jdoc[me].n = v; return me;
}
static int jparse(const char *text) { jp = text; return jvalue(); }
static int jget(int o, const char *k) {
  if (o < 0 || jdoc[o].t != J_OBJ) return -1;
  for (int c = jdoc[o].child; c >= 0; c = jdoc[c].next) if (!strcmp(jdoc[c].key, k)) return c;
  return -1;
}
static const char *jstr(int n, const char *d = "") { return n >= 0 && jdoc[n].t == J_STR ? jdoc[n].s : d; }
static const char *jgs(int o, const char *k, const char *d = "") { return jstr(jget(o, k), d); }

// ---- the open pack
static char loaded[32] = "";
static int edRoot = -1, cardsRoot = -1;
static int cardN[600], nCards = 0;
static const char *secId[32], *secTitle[32]; static int nSecs = 0;
static int inSec[600], nIn = 0;                 // the cards of the chosen section, in order
static bool loadPack(const char *slug) {
  if (!strcmp(loaded, slug) && edRoot >= 0) return true;
  int b = builtinIdx(slug); if (b < 0) return false;
  jfree(); loaded[0] = 0; nCards = nSecs = 0;
  size_t need = strlen(BUILTIN[b].edition) + strlen(BUILTIN[b].cards) + 64;
  jarena = (char *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM); if (!jarena) return false;
  edRoot = jparse(BUILTIN[b].edition); cardsRoot = jparse(BUILTIN[b].cards);
  if (edRoot < 0 || cardsRoot < 0 || jdoc[cardsRoot].t != J_ARR) { jfree(); edRoot = -1; return false; }
  for (int c = jdoc[cardsRoot].child; c >= 0 && nCards < 600; c = jdoc[c].next) cardN[nCards++] = c;
  int s = jget(edRoot, "sections");
  if (s >= 0) for (int c = jdoc[s].child; c >= 0 && nSecs < 32; c = jdoc[c].next) { secId[nSecs] = jgs(c, "id"); secTitle[nSecs] = jgs(c, "title"); nSecs++; }
  strncpy(loaded, slug, sizeof loaded - 1);
  return true;
}
static void pickSection(int sec) {
  nIn = 0;
  for (int i = 0; i < nCards; i++) if (sec < 0 || !strcmp(jgs(cardN[i], "section"), secId[sec])) inSec[nIn++] = i;
}
static void ensurePack() {                      // after a sleep: the open pack and section, parsed again
  bool needs = (R.scr == SCR_LIST && (R.list == L_SECS || R.list == L_CARDS)) || R.scr == SCR_CARD;
  if (!needs || !R.slug[0]) return;
  bool fresh = strcmp(loaded, R.slug) != 0 || edRoot < 0;
  if (!loadPack(R.slug)) return;
  if (fresh || nIn == 0) pickSection(R.sec);
}
static const char *packName() { int b = builtinIdx(R.slug); return b >= 0 ? BUILTIN[b].name : ""; }

// ================================================================ catalog: the InvisibleChaos stations, as big codes
struct Station { const char *code, *name, *url; };
static const Station STATIONS[] = {
  { "CV", "Resume", "https://cv-julien-le-tyrant.netlify.app/" },
  { "CV-ZH", "Resume, Chinese", "https://cv-julien-zh.netlify.app/" },
  { "EDIT", "E!nkster Editions", "https://einkster-editions.netlify.app/" },
  { "IC", "InvisibleChaos", "https://invisiblechaos.netlify.app/" },
  { "QLC", "Quellcode", "https://quellcode-berlin.netlify.app/" },
  { "TUNE", "TunerBro", "https://tunerbro.netlify.app/" },
  { "DDD", "DerDieDas Bro", "https://derdiedas-bro.netlify.app/" },
  { "WASSER", "Wasserbro", "https://wasserbro.netlify.app/" },
  { "DIY", "La Carte", "https://ic-kitchen.netlify.app/" },
  { "RDZ", "Radzeugnis", "https://radzeugnis.netlify.app/" },
  { "OCEAN", "!nvisibleOcean", "https://invisibleocean.netlify.app/" },
  { "SPL", "Spielhalle", "https://invisiblechaos.netlify.app/games.html" },
  { "VOLCANO", "VolcanoBro", "https://volcanobro.netlify.app/" },
  { "MET", "MeteoBro", "https://meteobro.netlify.app/" },
  { "FPN", "Fipness", "https://fipness.netlify.app/" },
  { "KFS", "Kaffeesatz", "https://kaffeesatz.netlify.app/" },
  { "ALT", "Altname", "https://altname.netlify.app/" },
  { "CIM", "ChaosInMotion", "https://chaosinmotion.netlify.app/" },
  { "KMD", "Kamerad", "https://kamerad.netlify.app/" },
  { "NBSP", "Ins\xC3\xA9" "cable", "https://insecable.netlify.app/" },
  { "LUC", "Lucible", "https://lucible.netlify.app/" },
  { "HUD", "Field HUD", "https://recumbenthud.netlify.app/" },
  { "SURV", "The Survivalist", "https://thesurvivalist.netlify.app/" },
  { "RDF", "Radfunk!", "https://radfunk.netlify.app/" },
};
#define N_STATIONS (int)(sizeof STATIONS / sizeof STATIONS[0])
static bool cardOn() { return strchr(EK_CARD, '|') != NULL; }      // the business card, from the build secret only
static String cardField(int k) { String s = EK_CARD; for (int i = 0; i < k; i++) { int p = s.indexOf('|'); if (p < 0) return ""; s = s.substring(p + 1); } int p = s.indexOf('|'); return p < 0 ? s : s.substring(0, p); }
static int catCount() { return N_STATIONS + (cardOn() ? 1 : 0); }
static int qrSize = 0; static uint8_t qrM[57 * 57];
static void qrGrab(esp_qrcode_handle_t q) {
  qrSize = esp_qrcode_get_size(q); if (qrSize > 57) { qrSize = 0; return; }
  for (int y = 0; y < qrSize; y++) for (int x = 0; x < qrSize; x++) qrM[y * qrSize + x] = esp_qrcode_get_module(q, x, y);
}
static bool drawQr(const String &text, int cx, int cy, int box) {
  esp_qrcode_config_t cfg = { qrGrab, 12, ESP_QRCODE_ECC_MED };
  qrSize = 0;
  if (esp_qrcode_generate(&cfg, text.c_str()) != ESP_OK || !qrSize) return false;
  int sc = box / (qrSize + 4); if (sc < 1) sc = 1;
  int side = qrSize * sc, x0 = cx - side / 2, y0 = cy - side / 2;
  for (int r = 0; r < qrSize; r++) for (int c = 0; c < qrSize; c++) if (qrM[r * qrSize + c]) rect(x0 + c * sc, y0 + r * sc, sc, sc, true);
  return true;
}

// ================================================================ screens
static const char *tileName(int t);
// ================================================================ the 30-minute log (v0.3.0, as on the 154)
// Every 30 minutes, asleep or awake: a 16-byte sample in /log.bin (the 154's layout). The battery page under Device draws
// the last 7 days and the drain since the last charge; the set-up page gives the whole log as a CSV file.
// Columns as the 154's, the fifth holds the 397's battery percentage (the power chip's own estimate) where the 154 counts asks.
#define LOGF_COLD   1                          // taken at a power-on start
#define LOGF_USB    4                          // USB in
#define LOGF_CHG    8                          // charging (constant current)
#define LOGF_TOPUP  16                         // topping up (constant voltage, the current falling to the 25 mA end)
#define LOGF_FULL   32                         // the charger has stopped: full
#define LOGF_KB     64                         // the keyboard was connected since the last sample
#define LOGF_AWAKE  128                        // taken while awake (typing, reading, set up), not at a timer wake
#define LOG_ROTATE  384000                     // about 16 months of samples, then the file moves to /log.old
static const char *chgWord(uint8_t c) {
  switch (c) { case 1: return L("charging", "en charge"); case 2: return L("topping up", "fin de charge"); case 3: return L("full", "pleine"); default: return ""; }
}
static String powerText() {
  if (!R.usb) return L("battery", "batterie");
  const char *w = chgWord(R.chg); return *w ? String("USB, ") + w : String("USB");
}
static void batLogWrite(bool cold, bool awake) {
  if (!timeValid() || !R.batMv) return;        // no clock, no cell: nothing worth a line
  BatSample s; memset(&s, 0, sizeof s);
  s.t = nowS(); s.mv = R.batMv; s.wifi = R.wifiN; s.asks = R.batPct >= 0 ? (uint8_t)R.batPct : 255;
  s.tin10 = R.roomT10 > -990 ? R.roomT10 : INT16_MIN; s.rh = R.roomH >= 0 ? (uint8_t)R.roomH : 255;
  s.tout = wxFresh() && R.outT10 > -990 ? (int8_t)constrain(lroundf(R.outT10 / 10.0f), -127, 127) : -128;
  s.p10 = 0;
  s.spare = (cold ? LOGF_COLD : 0) | (R.usb ? LOGF_USB : 0) | (R.chg == 1 ? LOGF_CHG : 0) | (R.chg == 2 ? LOGF_TOPUP : 0) | (R.chg == 3 ? LOGF_FULL : 0) |
            (R.kbOn ? LOGF_KB : 0) | (awake ? LOGF_AWAKE : 0);
  if (fsSize("/log.bin") > LOG_ROTATE) fsRotate("/log.bin", "/log.old");
  fsAppend("/log.bin", (const char *)&s, sizeof s);
}
static bool batCold = false;
// one sample: the field log line and the 30-minute record
static void sampleNow(bool awake) {
  R.lastSample = nowS();
  FL("battery", (R.batMv ? mvText(R.batMv) : String("no cell")) + (R.batPct >= 0 ? ", " + String(R.batPct) + " %" : "") + (R.usb ? ", " + powerText() : String("")) +
                (R.roomT10 > -990 ? " \xC2\xB7 room " + deg10(R.roomT10) + " " + String(R.roomH) + " %" : ""));
  batLogWrite(batCold, awake); batCold = false;
  R.wifiN = 0; R.kbOn = 0;
}
// while awake for long (typing, reading): the sample still comes every 30 minutes
static void sampleIfDue() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 30000) return;
  lastCheck = millis();
  if (nowS() - R.lastSample >= 1790) { readBattery(); readRoomNow(); sampleNow(true); }
}

// ---- the battery page
static BatSample *logLoad(int &n) {
  n = 0; uint8_t *b = NULL; size_t sz = fsRead("/log.bin", &b);
  n = sz / sizeof(BatSample);
  return (BatSample *)b;
}
static void drawBat() {
  readBattery();
  textLeft(EK_BIG, 20, 20, L("Battery", "Batterie"));
  rect(16, 70, W - 32, 3, true);
  textLeft(EK_BIG, 28, 90, R.batMv ? mvText(R.batMv) : String(L("no cell", "pas de cellule")));
  if (R.batPct >= 0) textRight(EK_BIG, W - 28, 90, String(R.batPct) + " %");
  textLeft(EK_MID, 28, 140, powerText());
  int n; BatSample *a = logLoad(n);
  // the last 7 days, 3.30 to 4.25 V
  const int GX = 70, GY = 190, GW = W - 100, GH = 240;
  rect(GX, GY + GH, GW, 2, true); rect(GX, GY, 2, GH, true);
  for (int v = 3400; v <= 4200; v += 200) { int y = GY + GH - (v - 3300) * GH / 950; for (int x = GX; x < GX + GW; x += 8) rect(x, y, 3, 1, true); textRight(EK_SMALL, GX - 6, y - 8, String(v / 1000.0f, 1)); }
  uint32_t now = nowS(), from = now - 7UL * 86400;
  for (int d = 1; d < 7; d++) rect(GX + GW * d / 7, GY + GH - 6, 2, 6, true);
  textLeft(EK_SMALL, GX, GY + GH + 6, L("7 days ago", "il y a 7 jours")); textRight(EK_SMALL, GX + GW, GY + GH + 6, L("now", "maintenant"));
  for (int i = 0; i < n; i++) {
    if (a[i].t < from || a[i].t > now + 600) continue;
    int x = GX + (int)((uint64_t)(a[i].t - from) * (GW - 1) / (7UL * 86400));
    int y = GY + GH - 1 - constrain(((int)a[i].mv - 3300) * (GH - 1) / 950, 0, GH - 1);
    rect(x - 1, y - 1, 3, 3, true);
    if (a[i].spare & LOGF_USB) rect(x - 1, GY + GH + 3, 3, 2, true);       // USB in: a mark under the axis
  }
  // the drain since the last charge: a straight line through the percentages
  int s0 = 0;
  for (int i = 0; i < n; i++) if ((a[i].spare & LOGF_USB) || (i && a[i].asks != 255 && a[i - 1].asks != 255 && a[i].asks > a[i - 1].asks + 3)) s0 = i + 1;
  String l1, l2, l3, l4;
  int m = 0; double sx = 0, sy = 0, sxx = 0, sxy = 0; uint32_t t0 = s0 < n ? a[s0].t : 0, t1 = t0;
  for (int i = s0; i < n; i++) { if (a[i].asks == 255) continue; double x = (a[i].t - t0) / 86400.0, y = a[i].asks; sx += x; sy += y; sxx += x * x; sxy += x * y; m++; t1 = a[i].t; }
  double span = m ? (t1 - t0) / 86400.0 : 0;
  if (R.usb) l1 = L("On USB: the drain is measured once it runs on its battery.", "Sur USB\xC2\xA0: la d\xC3\xA9" "charge se mesure sur batterie.");
  else if (m < 2 || span < 0.25) l1 = L("Since the last charge: needs 6 hours of samples.", "Depuis la derni\xC3\xA8re charge\xC2\xA0: il faut 6 heures de mesures.");
  else {
    double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);           // % per day, negative
    if (slope < -0.05) {
      l1 = String(L("Since the last charge: ", "Depuis la derni\xC3\xA8re charge\xC2\xA0: ")) + String(-slope, 1) + L(" %/day over ", " %/jour sur ") + String(span, 1) + L(" days", " jours");
      double left = (R.batPct >= 0 ? R.batPct : 0) / -slope;
      l2 = left < 1 ? String(L("about ", "environ ")) + (int)(left * 24) + L(" hours left at this rate", " heures restantes \xC3\xA0 ce rythme")
                    : String(L("about ", "environ ")) + (int)left + L(" days left at this rate", " jours restants \xC3\xA0 ce rythme");
    } else l1 = String(L("Since the last charge: barely moving over ", "Depuis la derni\xC3\xA8re charge\xC2\xA0: presque immobile sur ")) + String(span, 1) + L(" days", " jours");
  }
  // the last top-up: from the first "topping up" sample to the first "full" one after it
  for (int f = n - 1; f >= 0; f--) if (a[f].spare & LOGF_FULL) {
    int j = f; while (j > 0 && (a[j - 1].spare & (LOGF_FULL | LOGF_TOPUP))) j--;   // the start of this charge's end
    int k = j; while (k <= f && !(a[k].spare & LOGF_TOPUP)) k++;                   // its first "topping up" sample
    int ff = k; while (ff <= f && !(a[ff].spare & LOGF_FULL)) ff++;                // the first "full" one after it
    if (k <= f && ff <= f) l3 = String(L("Last top-up, 4.20 V to full: about ", "Derni\xC3\xA8re fin de charge, de 4,20 V \xC3\xA0 pleine\xC2\xA0: environ ")) + (int)((a[ff].t - a[k].t) / 60) + L(" min (samples every 30 min)", " min (mesures toutes les 30 min)");
    break;
  }
  l4 = String(n) + (n == 1 ? L(" sample", " mesure") : L(" samples", " mesures"));
  if (n) { char b[48]; time_t e = a[0].t; struct tm t; localtime_r(&e, &t); strftime(b, sizeof b, "%d.%m.%Y", &t); l4 += String(L(" since ", " depuis le ")) + b; }
  l4 += L("  \xC2\xB7  the CSV is on the set-up page", "  \xC2\xB7  le CSV est sur la page de r\xC3\xA9glages");
  int y = GY + GH + 40;
  y += para(EK_MID, 28, y, W - 56, l1, 3) + 8;
  if (l2.length()) y += para(EK_MID, 28, y, W - 56, l2, 2) + 8;
  if (l3.length()) y += para(EK_MID, 28, y, W - 56, l3, 3) + 8;
  para(EK_SMALL, 28, y + 8, W - 56, l4, 3);
  if (a) free(a);
  textAt(EK_SMALL, W / 2, H - 28, L("press: back", "appui\xC2\xA0: retour"));
}
// one sample as a line of the CSV (the set-up page)
#define LOG_CSV_HEAD "time_utc,epoch,battery_mv,wifi,battery_pct,room_c,room_rh,out_c,pressure_hpa,flags\n"
static int logCsvLine(const BatSample &s, char *b, size_t cap) {
  time_t e = s.t; struct tm t; gmtime_r(&e, &t);
  char room[12] = "", rh[8] = "", out[8] = "", pct[8] = "";
  if (s.tin10 != INT16_MIN) snprintf(room, sizeof room, "%s%d.%d", s.tin10 < 0 ? "-" : "", abs(s.tin10) / 10, abs(s.tin10) % 10);
  if (s.rh != 255) snprintf(rh, sizeof rh, "%u", (unsigned)s.rh);
  if (s.tout != -128) snprintf(out, sizeof out, "%d", (int)s.tout);
  if (s.asks != 255) snprintf(pct, sizeof pct, "%u", (unsigned)s.asks);
  return snprintf(b, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ,%lu,%u,%u,%s,%s,%s,%s,,%u\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec, (unsigned long)s.t, (unsigned)s.mv, (unsigned)s.wifi, pct, room, rh, out, (unsigned)s.spare);
}

static const char *tileName(int t) {
  switch (t) {
    case T_CLOCK: return L("Clock", "Horloge"); case T_CLIMATE: return L("Climate", "Climat");
    case T_EDITIONS: return "Editions"; case T_TYPE: return L("Typewriter", "Machine \xC3\xA0 \xC3\xA9" "crire"); case T_SHELF: return L("Archives", "Archives"); case T_CATALOG: return L("Catalog", "Catalogue"); default: return L("Device", "Appareil");
  }
}
static void batteryMark(int xr, int y) {        // top right: a cell and its percentage
  if (R.batPct < 0 && !R.usb) return;
  int w = 34, h = 18, x = xr - w - 4;
  frame(x, y, w, h, 2); rect(x + w, y + 5, 4, 8, true);
  if (R.batPct >= 0) rect(x + 3, y + 3, (w - 6) * constrain(R.batPct, 0, 100) / 100, h - 6, true);
  String p = R.batPct >= 0 ? String(R.batPct) + "%" : String("USB");
  if (R.charging) p = "+" + p;
  textRight(EK_SMALL, x - 6, y, p);
}
static void topBar() {
  for (int i = 0; i < N_TILES; i++) { int cx = W / 2 - (N_TILES - 1) * 13 + i * 26; if (i == R.tile) disc(cx, 18, 7); else ring(cx, 18, 7, 2); }
  batteryMark(W - 8, 9);
}
static int32_t epochMin() { return (int32_t)(time(NULL) / 60); }
static void dateLine(char *b, size_t n, time_t e) {
  static const char *WD_EN[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
  static const char *WD_FR[7] = { "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi" };
  static const char *MO_EN[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
  static const char *MO_FR[12] = { "janvier", "f\xC3\xA9vrier", "mars", "avril", "mai", "juin", "juillet", "ao\xC3\xBBt", "septembre", "octobre", "novembre", "d\xC3\xA9" "cembre" };
  struct tm t; localtime_r(&e, &t);
  if (uiFr) snprintf(b, n, "%s %d %s", WD_FR[t.tm_wday], t.tm_mday, MO_FR[t.tm_mon]);
  else snprintf(b, n, "%s %d %s", WD_EN[t.tm_wday], t.tm_mday, MO_EN[t.tm_mon]);
}
static void faceClock() {
  topBar();
  if (R.faceMin <= 0) { textAt(EK_BIG, W / 2, 230, L("Clock not set", "Horloge non r\xC3\xA9gl\xC3\xA9" "e")); para(EK_MID, 40, 290, W - 80, L("Device > Set up: the clock comes from your phone, or from the internet with a known network.", "Appareil > R\xC3\xA9glages\xC2\xA0: l'heure vient du t\xC3\xA9l\xC3\xA9phone, ou d'internet avec un r\xC3\xA9seau connu."), 8, true); }
  else {
    time_t e = (time_t)R.faceMin * 60; struct tm t; localtime_r(&e, &t);
    int dw = 86, dh = 172, k = 20, gap = 14, colon = 34, x = (W - (4 * dw + 2 * gap + colon + gap)) / 2, y = 110;
    digit(x, y, t.tm_hour / 10, dw, dh, k); digit(x + dw + gap, y, t.tm_hour % 10, dw, dh, k);
    int cx = x + 2 * dw + gap + gap / 2 + colon / 2; rect(cx - 9, y + dh / 3 - 9, 18, 18, true); rect(cx - 9, y + 2 * dh / 3 - 9, 18, 18, true);
    int x2 = x + 2 * dw + gap + colon + gap;
    digit(x2, y, t.tm_min / 10, dw, dh, k); digit(x2 + dw + gap, y, t.tm_min % 10, dw, dh, k);
    char b[64]; dateLine(b, sizeof b, e); textAt(EK_BIG, W / 2, 318, b);
  }
  rect(30, 380, W - 60, 3, true);
  // the weather, if it came within 3 hours
  int code = R.wxOk ? R.outCode : -1;
  if (!R.wxOk) { textAt(EK_BIG, W / 2, 420, "--", 2); textAt(EK_MID, W / 2, 510, codeWord(-1)); }
  else {
    wxMark(code, 110, 470, 3);
    textLeft(EK_BIG, 220, 420, deg10(R.outT10), true, W - 230, 2);
    textLeft(EK_MID, 220, 500, codeWord(code), true, W - 230);
  }
  if (R.wxOk) textLeft(EK_MID, 220, 530, String(L("today ", "aujourd'hui ")) + degInt(R.dMin10[0]) + " / " + degInt(R.dMax10[0]), true, W - 230);
  rect(30, 590, W - 60, 3, true);
  textLeft(EK_MID, 40, 615, L("Room", "Pi\xC3\xA8" "ce"));
  textLeft(EK_BIG, 40, 645, R.roomT10 > -990 ? deg10(R.roomT10) : String("--"));
  if (R.roomH >= 0) textRight(EK_BIG, W - 40, 645, String(R.roomH) + " %");
  textAt(EK_SMALL, W / 2, H - 30, String(tileName(R.tile)) + "  \xC2\xB7  E!nkster 397");
}
static void faceClimate() {
  topBar();
  textAt(EK_BIG, W / 2, 50, tileName(T_CLIMATE));
  textLeft(EK_MID, 40, 120, L("Room", "Pi\xC3\xA8" "ce"));
  textLeft(EK_BIG, 40, 150, R.roomT10 > -990 ? deg10(R.roomT10) : String("--"), true, 300, 2);
  textLeft(EK_MID, 40, 230, R.roomH >= 0 ? String(L("humidity ", "humidit\xC3\xA9 ")) + R.roomH + " %" : String(L("no room sensor", "pas de capteur")));
  rect(30, 280, W - 60, 3, true);
  textLeft(EK_MID, 40, 300, L("Outside", "Dehors"));
  int code = R.wxOk ? R.outCode : -1;
  if (!R.wxOk) { textAt(EK_BIG, W / 2, 350, "--", 2); textAt(EK_MID, W / 2, 440, codeWord(-1)); }
  else {
    wxMark(code, 110, 400, 3);
    textLeft(EK_BIG, 220, 350, deg10(R.outT10), true, W - 230, 2);
    textLeft(EK_MID, 220, 430, codeWord(code), true, W - 230);
  }
  rect(30, 500, W - 60, 3, true);
  if (R.wxOk) {
    for (int d = 0; d < 3; d++) {
      int cx = 80 + d * 160;
      time_t e = (time_t)R.wxAt + d * 86400; struct tm t; localtime_r(&e, &t);
      static const char *D_EN[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }, *D_FR[7] = { "dim", "lun", "mar", "mer", "jeu", "ven", "sam" };
      textAt(EK_MID, cx, 525, d == 0 ? String(L("today", "auj.")) : String(uiFr ? D_FR[t.tm_wday] : D_EN[t.tm_wday]));
      wxMark(R.dCode[d], cx, 600, 1);
      textAt(EK_MID, cx, 650, degInt(R.dMax10[d]));
      textAt(EK_MID, cx, 680, degInt(R.dMin10[d]));
    }
    time_t e = R.wxAt; struct tm t; localtime_r(&e, &t); char b[48]; snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min);
    textAt(EK_SMALL, W / 2, 730, String(L("Open-Meteo, at ", "Open-Meteo, \xC3\xA0 ")) + b);
  } else para(EK_MID, 40, 540, W - 80, R.nets ? L("The forecast comes with the next hourly visit online.", "Les pr\xC3\xA9visions arrivent \xC3\xA0 la prochaine connexion, chaque heure.")
                                            : L("No network known yet: add one under Device > Set up.", "Aucun r\xC3\xA9seau connu\xC2\xA0: ajoutez-en un dans Appareil > R\xC3\xA9glages."), 6, true);
  textAt(EK_SMALL, W / 2, H - 30, L("press: fetch the weather now", "appui\xC2\xA0: m\xC3\xA9t\xC3\xA9o maintenant"));
}
static void bookMark(int cx, int cy, int s) {
  rect(cx - 30 * s, cy - 20 * s, 28 * s, 40 * s, true); rect(cx + 2 * s, cy - 20 * s, 28 * s, 40 * s, true);
  rect(cx - 27 * s, cy - 17 * s, 23 * s, 34 * s, false); rect(cx + 5 * s, cy - 17 * s, 23 * s, 34 * s, false);
  for (int i = 0; i < 4; i++) { rect(cx - 24 * s, cy + (-11 + i * 8) * s, 17 * s, 2 * s, true); rect(cx + 8 * s, cy + (-11 + i * 8) * s, 17 * s, 2 * s, true); }
}
static void faceEditions() {
  topBar();
  textAt(EK_BIG, W / 2, 50, "Editions");
  bookMark(W / 2, 220, 3);
  buildShelf();
  textAt(EK_MID, W / 2, 350, String(shelfN) + L(" packs on board, no card needed", " recueils \xC3\xA0 bord, sans carte"));
  int y = 410;
  for (int i = 0; i < shelfN && y < 700; i++, y += 32) textAt(EK_MID, W / 2, y, BUILTIN[shelf[i]].name);
  textAt(EK_SMALL, W / 2, H - 30, L("press: open the shelf", "appui\xC2\xA0: ouvrir l'\xC3\xA9tag\xC3\xA8re"));
}
static bool kbSavedQuick() { return prefs.getBytesLength("kbA") == 6; }
static void faceType() {
  topBar();
  textAt(EK_BIG, W / 2, 50, tileName(T_TYPE));
  rect(110, 160, 260, 120, true); rect(118, 168, 244, 104, false);                       // a keyboard, drawn
  for (int r = 0; r < 4; r++) for (int c = 0; c < (r == 3 ? 1 : 9); c++) rect(r == 3 ? 170 : 128 + c * 26 + (r % 2) * 8, 180 + r * 24, r == 3 ? 140 : 18, 16, true);
  int y = 330;
  y += para(EK_MID, 40, y, W - 80, L("A Bluetooth keyboard, the page turned on its side. Enter saves the line to the microSD, in a file for the day. Saved lines are never edited.",
                                    "Un clavier Bluetooth, la page tourn\xC3\xA9" "e sur le c\xC3\xB4t\xC3\xA9. Entr\xC3\xA9" "e enregistre la ligne sur la microSD, un fichier par jour. Les lignes enregistr\xC3\xA9" "es ne se modifient plus."), 8, true) + 24;
  uint8_t a[6], t;
  textAt(EK_MID, W / 2, y, kbSavedQuick() ? (R.kbBat >= 0 ? String(L("keyboard paired, battery ", "clavier appair\xC3\xA9, batterie ")) + (int)R.kbBat + " %" : String(L("keyboard paired", "clavier appair\xC3\xA9")))
                                          : String(L("no keyboard paired yet", "pas encore de clavier")));
  (void)a; (void)t;
  textAt(EK_SMALL, W / 2, H - 30, L("press: start writing", "appui\xC2\xA0: \xC3\xA9" "crire"));
}
static void faceCatalog() {
  topBar();
  textAt(EK_BIG, W / 2, 50, tileName(T_CATALOG));
  drawQr("https://invisiblechaos.netlify.app/", W / 2, 330, 360);
  textAt(EK_MID, W / 2, 540, "invisiblechaos.netlify.app");
  para(EK_MID, 40, 600, W - 80, String(catCount()) + L(" links, each as a big code to share face to face.", " liens, chacun en grand code \xC3\xA0 partager de vive voix."), 4, true);
  textAt(EK_SMALL, W / 2, H - 30, L("press: open the list", "appui\xC2\xA0: ouvrir la liste"));
}
static void faceDevice() {
  topBar();
  textAt(EK_BIG, W / 2, 50, "E!nkster 397");
  textAt(EK_MID, W / 2, 100, String(EK_VERSION) + "  \xC2\xB7  " + devName());
  int y = 170;
  auto row = [&](const String &a, const String &b) { textLeft(EK_MID, 40, y, a); textRight(EK_MID, W - 40, y, b); y += 40; };
  row(L("Battery", "Batterie"), R.batMv ? mvText(R.batMv) + (R.batPct >= 0 ? "  " + String(R.batPct) + " %" : "") : String(L("no cell", "pas de cellule")));
  row(L("Power", "Alimentation"), powerText());   // v0.3.0: charging, topping up, full
  row(L("Networks", "R\xC3\xA9seaux"), String(R.nets));
  row(L("Clock", "Horloge"), R.clockOk ? (R.ntpAt ? L("internet", "internet") : L("set", "r\xC3\xA9gl\xC3\xA9" "e")) : L("not set", "non r\xC3\xA9gl\xC3\xA9" "e"));
  row(L("Language", "Langue"), L("English", "fran\xC3\xA7" "ais"));
  row(L("Keyboard", "Clavier"), kbSavedQuick() ? (R.kbBat >= 0 ? String((int)R.kbBat) + " %" : String(L("paired", "appair\xC3\xA9"))) : String(L("none", "aucun")));   // v0.2.0
  rect(30, y + 10, W - 60, 3, true);
  para(EK_MID, 40, y + 40, W - 80, L("Press for the menu: set up, language, the keyboard, about, the battery log, switch off.", "Appui pour le menu\xC2\xA0: r\xC3\xA9glages, langue, clavier, \xC3\xA0 propos, journal de batterie, \xC3\xA9teindre."), 4, true);
  textAt(EK_SMALL, W / 2, H - 30, L("dial: tiles  \xC2\xB7  long press: Clock", "molette\xC2\xA0: tuiles  \xC2\xB7  appui long\xC2\xA0: horloge"));
}

// ---- lists
static bool shScanned = false; static void shScan(); static std::vector<String> shDirs, shTitlesOf;   // v0.3.0: the archives on the card
#define ROW_H 46
#define LIST_Y 90
#define LIST_ROWS 14
static int listCount() {
  switch (R.list) {
    case L_SHELF: buildShelf(); return shelfN;
    case L_SECS: return loadPack(R.slug) ? nSecs + 1 : 0;
    case L_CARDS: return nIn;
    case L_CATALOG: return catCount();
    case L_ARCH: if (!shScanned) shScan(); return shDirs.size();
    default: return 8;
  }
}
static String listItem(int i, String &right) {
  right = "";
  switch (R.list) {
    case L_SHELF: { const Builtin &b = BUILTIN[shelf[i]]; if (b.lang[0]) right = b.lang; return b.name; }
    case L_SECS: if (i < nSecs) return secTitle[i]; right = String(nCards); return L("All cards", "Toutes les fiches");
    case L_CARDS: { int c = cardN[inSec[i]]; const char *ty = jgs(c, "type"); if (!strcmp(ty, "prompt")) right = "?"; else if (!strcmp(ty, "qr")) right = "QR"; else if (!strcmp(ty, "checklist")) right = "\xE2\x9C\x93"; return jgs(c, "title"); }
    case L_ARCH: if (!shScanned) shScan(); return i < (int)shTitlesOf.size() ? shTitlesOf[i] : String("");
    case L_CATALOG: if (cardOn() && i == 0) return L("Business card", "Carte de visite"); { const Station &s = STATIONS[i - (cardOn() ? 1 : 0)]; right = s.code; return s.name; }
    default: {
      static const char *const EN[8] = { "Set up: Wi-Fi, clock, field log", "Language: English", "Typewriter: turn the page", "Keyboard: forget it", "About and credits", "Battery: the 30-minute log", "Switch off", "Back" };
      static const char *const FR[8] = { "R\xC3\xA9glages\xC2\xA0: Wi-Fi, heure, journal", "Langue\xC2\xA0: fran\xC3\xA7" "ais", "Machine \xC3\xA0 \xC3\xA9" "crire\xC2\xA0: tourner la page", "Clavier\xC2\xA0: l'oublier", "\xC3\x80 propos et cr\xC3\xA9" "dits", "Batterie\xC2\xA0: le journal de 30 minutes", "\xC3\x89teindre", "Retour" };
      if (i == 2) right = prefs.getUChar("twRot", 1) == 2 ? "180\xC2\xB0" : "0\xC2\xB0";
      if (i == 3) { if (!kbSavedQuick()) return L("Keyboard: pair one", "Clavier\xC2\xA0: en appairer un"); right = R.kbBat >= 0 ? String((int)R.kbBat) + " %" : String(L("paired", "appair\xC3\xA9")); }   // v0.4.0
      return uiFr ? FR[i] : EN[i];
    }
  }
}
static String listTitle() {
  switch (R.list) {
    case L_SHELF: return "Editions";
    case L_SECS: return packName();
    case L_CARDS: return R.sec >= 0 && R.sec < nSecs ? String(secTitle[R.sec]) : String(packName());
    case L_CATALOG: return tileName(T_CATALOG);
    case L_ARCH: return tileName(T_SHELF);
    default: return tileName(T_DEVICE);
  }
}
static void drawList() {
  int n = listCount();
  if (R.cur >= n) R.cur = n ? n - 1 : 0;
  if (R.cur < R.top) R.top = R.cur;
  if (R.cur >= R.top + LIST_ROWS) R.top = R.cur - LIST_ROWS + 1;
  textLeft(EK_BIG, 20, 20, listTitle(), true, W - 130);
  textRight(EK_SMALL, W - 16, 32, String(R.cur + 1) + "/" + String(n));
  rect(16, 70, W - 32, 3, true);
  for (int r = 0; r < LIST_ROWS && R.top + r < n; r++) {
    int i = R.top + r, y = LIST_Y + r * ROW_H; String right; String s = listItem(i, right);
    int rw = right.length() ? textWidth(EK_MID, right) + 16 : 0;
    textLeft(EK_MID, 24, y + 10, s, true, W - 48 - rw);
    if (rw) textRight(EK_MID, W - 24, y + 10, right);
    if (i == R.cur) invert(16, y + 2, W - 32, ROW_H - 4);
  }
  if (R.top > 0) for (int k = 0; k < 8; k++) rect(W / 2 - k, 76 + k, 2 * k + 1, 1, true);
  if (R.top + LIST_ROWS < n) for (int k = 0; k < 8; k++) rect(W / 2 - k, H - 40 - k, 2 * k + 1, 1, true);
  textAt(EK_SMALL, W / 2, H - 28, L("dial: move  \xC2\xB7  press: open  \xC2\xB7  long press: back", "molette\xC2\xA0: choisir  \xC2\xB7  appui\xC2\xA0: ouvrir  \xC2\xB7  long\xC2\xA0: retour"));
}

// ---- a card, in pages
#define CARD_X 28
#define CARD_W (W - 56)
#define BODY_TOP 120
#define BODY_BOTTOM (H - 60)
struct Line { int16_t a, b; uint8_t kind; };   // kind 0 body (MID), 1 item, 2 answer, 3 gap, 4 label
static Line lines[400]; static int nLines = 0;
static uint16_t cardText[6000]; static int cardLen = 0;
static bool cardBold[6000];                      // **bold** in a card: drawn underlined, the font is already bold (as on the 154)
static int pageStart[40], nPages = 0;
static void addText(const char *s, int kind) {
  int n = 0, room = (int)(sizeof cardText / 2) - cardLen - 1; bool bold = false;
  const uint8_t *p = (const uint8_t *)s;
  while (*p && n < room) {
    if (p[0] == '*' && p[1] == '*') { bold = !bold; p += 2; continue; }
    int len = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1;
    char ch[5] = { 0 }; for (int i = 0; i < len && p[i]; i++) ch[i] = p[i];
    p += strlen(ch) ? strlen(ch) : 1;
    uint16_t cp[2]; if (decodeUtf8(ch, cp, 2) < 1) continue;
    cardText[cardLen + n] = cp[0]; cardBold[cardLen + n] = bold; n++;
  }
  static int st[200], en[200];
  int w = kind == 1 ? CARD_W - 40 : CARD_W;
  int k = wrap(EK_MID, cardText + cardLen, n, w, st, en, 200);
  for (int i = 0; i < k && nLines < 400; i++) lines[nLines++] = { (int16_t)(cardLen + st[i]), (int16_t)(cardLen + en[i]), (uint8_t)(kind == 1 && i > 0 ? 5 : kind) };
  cardLen += n;
}
static void addGap() { if (nLines < 400) lines[nLines++] = { 0, 0, 3 }; }
static void layoutCard() {
  nLines = 0; cardLen = 0;
  int c = cardN[inSec[R.card]]; const char *ty = jgs(c, "type");
  if (!strcmp(ty, "prompt")) {
    addText(jgs(c, "question"), 0); addGap();
    if (R.revealed) { addText(L("Answer", "R\xC3\xA9ponse"), 4); addText(jgs(c, "answer"), 2); }
  } else if (!strcmp(ty, "checklist")) {
    int it = jget(c, "items"); if (it >= 0) for (int x = jdoc[it].child; x >= 0; x = jdoc[x].next) addText(jstr(x), 1);
  } else if (!strcmp(ty, "qr")) {
    addText(jgs(c, "text"), 0);
  } else {
    // body paragraphs keep their blank line
    const char *b = jgs(c, "body"); addText(b, 0);
  }
  // pages
  nPages = 0; int y = 0, maxH = BODY_BOTTOM - BODY_TOP;
  if (!strcmp(ty, "qr")) maxH -= 330;
  pageStart[nPages++] = 0;
  for (int i = 0; i < nLines; i++) {
    int h = lines[i].kind == 3 ? EK_MID.height / 2 : EK_MID.height + 4;
    if (y + h > maxH && i > pageStart[nPages - 1] && nPages < 40) { pageStart[nPages++] = i; y = 0; }
    y += h;
  }
  if (R.page >= nPages) R.page = nPages - 1;
  if (R.page < 0) R.page = 0;
}
static void drawCard() {
  if (!loadPack(R.slug) || R.card < 0 || R.card >= nIn) { textAt(EK_BIG, W / 2, 300, L("Card not found", "Fiche introuvable")); return; }
  layoutCard();
  int c = cardN[inSec[R.card]]; const char *ty = jgs(c, "type");
  textLeft(EK_SMALL, 20, 14, packName(), true, W - 150);
  textRight(EK_SMALL, W - 16, 14, String(R.card + 1) + "/" + String(nIn));
  int th = para(EK_BIG, 20, 44, W - 40, jgs(c, "title"), 2);
  rect(16, 48 + th, W - 32, 3, true);
  int y = max(BODY_TOP, 60 + th), end = R.page + 1 < nPages ? pageStart[R.page + 1] : nLines;
  for (int i = pageStart[R.page]; i < end; i++) {
    const Line &ln = lines[i];
    if (ln.kind == 3) { y += EK_MID.height / 2; continue; }
    int x = CARD_X;
    if (ln.kind == 1) { frame(CARD_X, y + 4, 18, 18, 2); x += 40; }
    if (ln.kind == 5) x += 40;
    if (ln.kind == 4) { rect(CARD_X, y + EK_MID.height + 1, textWidth(EK_MID, L("Answer", "R\xC3\xA9ponse")), 2, true); }
    for (int k = ln.a; k < ln.b; k++) {
      int gw = glyphW(EK_MID, cardText[k]); glyph(EK_MID, x, y, cardText[k], true);
      if (cardBold[k]) rect(x, y + EK_MID.ascent + 3, gw, 2, true);
      x += gw;
    }
    y += EK_MID.height + 4;
  }
  if (!strcmp(ty, "qr") && R.page == nPages - 1) {
    const char *u = jgs(c, "url");
    if (!drawQr(u, W / 2, BODY_BOTTOM - 165, 300)) textAt(EK_MID, W / 2, BODY_BOTTOM - 180, L("link too long for a code", "lien trop long pour un code"));
  }
  String foot;
  if (!strcmp(ty, "prompt") && !R.revealed) foot = L("press: show the answer", "appui\xC2\xA0: voir la r\xC3\xA9ponse");
  else if (nPages > 1) foot = String(L("page ", "page ")) + (R.page + 1) + "/" + nPages + L("  \xC2\xB7  dial: turn", "  \xC2\xB7  molette\xC2\xA0: tourner");
  else foot = L("dial: next card  \xC2\xB7  long press: list", "molette\xC2\xA0: fiche suivante  \xC2\xB7  long\xC2\xA0: liste");
  textAt(EK_SMALL, W / 2, H - 28, foot);
}
static void drawCatQr() {
  int i = R.qr;
  if (cardOn() && i == 0) {
    String name = cardField(0), mail = cardField(1), tel = cardField(2);
    textAt(EK_BIG, W / 2, 30, name);
    String vc = "BEGIN:VCARD\nVERSION:3.0\nFN:" + name + "\nEMAIL:" + mail + "\nTEL:" + tel + "\nEND:VCARD";
    drawQr(vc, W / 2, 380, 440);
    textAt(EK_MID, W / 2, 640, mail); textAt(EK_MID, W / 2, 680, tel);
  } else {
    const Station &s = STATIONS[i - (cardOn() ? 1 : 0)];
    textAt(EK_BIG, W / 2, 30, s.name);
    textAt(EK_SMALL, W / 2, 80, s.code);
    if (!drawQr(s.url, W / 2, 380, 440)) textAt(EK_MID, W / 2, 380, L("link too long for a code", "lien trop long pour un code"));
    String u = s.url; if (u.startsWith("https://")) u = u.substring(8); if (u.endsWith("/")) u = u.substring(0, u.length() - 1);
    para(EK_MID, 20, 640, W - 40, u, 3, true);
  }
  textAt(EK_SMALL, W / 2, H - 28, L("dial: next code  \xC2\xB7  press or long press: back", "molette\xC2\xA0: code suivant  \xC2\xB7  appui\xC2\xA0: retour"));
}
static const char *ABOUT_EN =
  "E!nkster 397, v0.4.0. One of the E!nkster boards, with the 154, the 175 and the X4. Same tiles where they fit, same packs, same field log.\n\n"
  "Credits. Board, demo code and pin map: Waveshare (MIT). Power chip library: XPowersLib by Lewis He (MIT). "
  "Typeface: DejaVu Sans Bold (Bitstream Vera licence). QR codes: Espressif's esp_qrcode, built on Project Nayuki's QR Code generator (MIT). "
  "Weather: Open-Meteo (CC BY 4.0). Time: PTB and the NTP Pool. The packs: their authors, named on each pack's colophon card. "
  "Firmware: ESP32 Arduino core by Espressif. Keyboard: the core's BLE library (Neil Kolban, Espressif; Apache-2.0) on Espressif's Bluedroid (ESP-IDF, Apache-2.0). microSD: SD_MMC and FatFs (Espressif, ChaN). "
  "Archives: Kiwix and openZIM (the archives, libzim and python-libzim, GPL-3.0-or-later, used by the converter on a computer); each archive's source and licence on its articles.\n\n"
  "Open source, MIT. InvisibleChaos.\n\nWritten by Julien and Claude the AI";
static const char *ABOUT_FR =
  "E!nkster 397, v0.4.0. L'une des cartes E!nkster, avec la 154, la 175 et la X4. Les m\xC3\xAAmes tuiles quand elles s'y pr\xC3\xAAtent, les m\xC3\xAAmes recueils, le m\xC3\xAAme journal.\n\n"
  "Cr\xC3\xA9" "dits. Carte, code de d\xC3\xA9monstration et brochage\xC2\xA0: Waveshare (MIT). Puce d'alimentation\xC2\xA0: XPowersLib de Lewis He (MIT). "
  "Police\xC2\xA0: DejaVu Sans Bold (licence Bitstream Vera). Codes QR\xC2\xA0: esp_qrcode d'Espressif, sur le g\xC3\xA9n\xC3\xA9rateur de Project Nayuki (MIT). "
  "M\xC3\xA9t\xC3\xA9o\xC2\xA0: Open-Meteo (CC BY 4.0). Heure\xC2\xA0: PTB et le NTP Pool. Les recueils\xC2\xA0: leurs auteurs, nomm\xC3\xA9s sur la fiche colophon de chacun. "
  "Firmware\xC2\xA0: le c\xC5\x93ur Arduino ESP32 d'Espressif. Clavier\xC2\xA0: la biblioth\xC3\xA8que BLE du c\xC5\x93ur (Neil Kolban, Espressif\xC2\xA0; Apache-2.0) sur Bluedroid d'Espressif (ESP-IDF, Apache-2.0). microSD\xC2\xA0: SD_MMC et FatFs (Espressif, ChaN). "
  "Archives\xC2\xA0: Kiwix et openZIM (les archives, libzim et python-libzim, GPL-3.0 ou plus, utilis\xC3\xA9s par le convertisseur sur ordinateur)\xC2\xA0; la source et la licence de chaque archive sur ses articles.\n\n"
  "Open source, MIT. InvisibleChaos.\n\nWritten by Julien and Claude the AI";
static void drawDoc() {
  textLeft(EK_BIG, 20, 20, L("About", "\xC3\x80 propos"));
  rect(16, 70, W - 32, 3, true);
  para(EK_MID, CARD_X, 100, CARD_W, uiFr ? ABOUT_FR : ABOUT_EN, 26);
  textAt(EK_SMALL, W / 2, H - 28, L("press: back", "appui\xC2\xA0: retour"));
}
static String apName() { char b[24]; snprintf(b, sizeof b, "E!nkster-%s", devName().c_str() + 4); return String(b); }
static String apPass() {
  String pw = prefs.getString(EK_SET_HOTSPOT_PW, "");
  if (pw.length() == 8) return pw;
  static const char AB[] = "abcdefghjkmnpqrstuvwxyz23456789";
  pw = ""; for (int i = 0; i < 8; i++) pw += AB[esp_random() % (sizeof AB - 1)];
  prefs.putString(EK_SET_HOTSPOT_PW, pw);
  return pw;
}
static String setupNote;
static bool kbPoll(TwRep &r); static void twReport(const TwRep &r); static void escSink(uint8_t, uint8_t u); static bool navEsc = false;   // v0.4.0
static void drawSetup() {
  textLeft(EK_BIG, 20, 20, L("Set up", "R\xC3\xA9glages"));
  rect(16, 70, W - 32, 3, true);
  int y = 90;
  y += para(EK_MID, CARD_X, y, CARD_W, L("1. Join this hotspot with your phone (scan the code):", "1. Rejoignez ce point d'acc\xC3\xA8s avec le t\xC3\xA9l\xC3\xA9phone (code)\xC2\xA0:"), 3);
  drawQr("WIFI:T:WPA;S:" + apName() + ";P:" + apPass() + ";;", W / 2, y + 150, 290);
  y += 300;
  textAt(EK_MID, W / 2, y, apName()); y += 32;
  textAt(EK_MID, W / 2, y, String(L("password ", "mot de passe ")) + apPass()); y += 50;
  y += para(EK_MID, CARD_X, y, CARD_W, L("2. Open http://192.168.4.1 if the page does not open by itself.", "2. Ouvrez http://192.168.4.1 si la page ne s'ouvre pas seule."), 3) + 16;
  if (setupNote.length()) para(EK_MID, CARD_X, y, CARD_W, setupNote, 4);
  textAt(EK_SMALL, W / 2, H - 28, L("closes after 5 minutes without use  \xC2\xB7  long press: close", "se ferme apr\xC3\xA8s 5 minutes  \xC2\xB7  appui long\xC2\xA0: fermer"));
}
static void twUpright();
static void brand(const char *third) {
  twUpright();                                     // v0.2.0: always upright
  clearFb();
  textAt(EK_BIG, W / 2, 300, "E!NKSTER", 2);
  textAt(EK_BIG, W / 2, 400, "397", 2);
  textAt(EK_BIG, W / 2, 520, third);
}
static void drawType(); static void drawKb(); static void drawFind(); static void drawRead(); static void faceShelf();
static void render() {
  clearFb();
  uiFr = R.fr;
  ensurePack();
  switch (R.scr) {
    case SCR_TILE:
      if (R.tile == T_CLOCK) faceClock(); else if (R.tile == T_CLIMATE) faceClimate(); else if (R.tile == T_EDITIONS) faceEditions();
      else if (R.tile == T_TYPE) faceType(); else if (R.tile == T_SHELF) faceShelf(); else if (R.tile == T_CATALOG) faceCatalog(); else faceDevice();
      break;
    case SCR_LIST: drawList(); break;
    case SCR_CARD: drawCard(); break;
    case SCR_QR: drawCatQr(); break;
    case SCR_DOC: drawDoc(); break;
    case SCR_SETUP: drawSetup(); break;
    case SCR_TYPE: drawType(); break;           // v0.2.0
    case SCR_KB: drawKb(); break;
    case SCR_FIND: drawFind(); break;          // v0.3.0
    case SCR_READ: drawRead(); break;
    case SCR_BAT: drawBat(); break;
  }
}

// ================================================================ refresh
#define GHOST_CAP 60                           // partial refreshes before a clean one (the clock's hourly one comes first)
static bool havePrev = false;
static void push(bool forceFull) {
  bool full = forceFull || !havePrev || R.partials >= GHOST_CAP;
  if (!full && prevFb && !memcmp(prevFb, fb, FB_BYTES)) return;     // nothing moved
  epdPush(full, prevFb ? prevFb : fb);
  if (full) { R.partials = 0; R.lastFull = nowS(); } else R.partials++;
  if (prevFb) { memcpy(prevFb, fb, FB_BYTES); havePrev = true; }
}
// after a deep sleep the picture on the paper is redrawn from the state it was drawn from (RTC), so the next
// refresh can be partial: the controller needs both the old and the new picture.
static void restorePrev() {
  if (!prevFb) return;
  render(); memcpy(prevFb, fb, FB_BYTES); havePrev = true;
}

// ================================================================ network (the bench has none)
#ifndef EK_HOST
static void wifiOff() { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
static bool wifiConnect(uint32_t budgetMs) {
  nets.load(*kv); R.nets = nets.count;
  if (!nets.count) return false;
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(false);
  for (int k = 0; k < nets.count; k++) {
    int i = nets.order(k); if (i < 0) break;
    WiFi.begin(nets.name[i], nets.pass[i]);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < budgetMs) delay(50);
    if (WiFi.status() == WL_CONNECTED) { nets.worked(*kv, i); FL("wifi", "connected: network " + String(i + 1) + " of " + String(nets.count) + " in " + String((millis() - t0) / 1000.0f, 1) + " s"); return true; }
    WiFi.disconnect();
  }
  FL("wifi", "no known network answered (" + String(nets.count) + " known)");
  return false;
}
static volatile bool ntpFresh = false;
static void onNtp(struct timeval *) { ntpFresh = true; }
static void syncTime() {
  ntpFresh = false;
  sntp_set_time_sync_notification_cb(onNtp);
  configTzTime(tzCache.c_str(), "ptbtime1.ptb.de", "ptbtime2.ptb.de", "de.pool.ntp.org");
  uint32_t t0 = millis(); while (!ntpFresh && millis() - t0 < 6000) delay(50);
  if (ntpFresh && timeValid()) { systemToRtcChip(); R.ntpAt = nowS(); FL("time", "clock set from the internet"); }
  else FL("time", "no answer from the time servers");
}
static float jnumAfter(const String &b, int from, const char *key) {
  int k = b.indexOf(String("\"") + key + "\"", from); if (k < 0) return NAN;
  k = b.indexOf(':', k); if (k < 0) return NAN; k++;
  while (k < (int)b.length() && (b[k] == ' ' || b[k] == '[')) k++;
  if (b.startsWith("null", k)) return NAN;
  return b.substring(k).toFloat();
}
static void jarr(const String &b, int from, const char *key, float *out, int n) {
  for (int i = 0; i < n; i++) out[i] = NAN;
  int k = b.indexOf(String("\"") + key + "\"", from); if (k < 0) return;
  k = b.indexOf('[', k); if (k < 0) return; k++;
  for (int i = 0; i < n; i++) {
    while (k < (int)b.length() && b[k] == ' ') k++;
    if (b.startsWith("null", k)) out[i] = NAN; else out[i] = b.substring(k).toFloat();
    int c = b.indexOf(',', k), e = b.indexOf(']', k); if (c < 0 || (e >= 0 && e < c)) break; k = c + 1;
  }
}
static bool fetchWeather() {
  String lat = prefs.getString(EK_SET_LAT, "52.5200"), lon = prefs.getString(EK_SET_LON, "13.4050");   // personal: never logged
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + lat + "&longitude=" + lon +
               "&current=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=3&timezone=auto";
  NetworkClientSecure cli; cli.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  if (!http.begin(cli, url)) return false;
  uint32_t t0 = millis(); int code = http.GET();
  if (code != 200) { FL("fetch", "api.open-meteo.com: HTTP " + String(code)); http.end(); return false; }
  String b = http.getString(); http.end();
  int cur = b.indexOf("\"current\":{"); int day = b.indexOf("\"daily\":{");
  if (cur < 0 || day < 0) { FL("fetch", "api.open-meteo.com: no forecast in the answer"); return false; }
  float t = jnumAfter(b, cur, "temperature_2m"), c = jnumAfter(b, cur, "weather_code");
  float mx[3], mn[3], dc[3];
  jarr(b, day, "temperature_2m_max", mx, 3); jarr(b, day, "temperature_2m_min", mn, 3); jarr(b, day, "weather_code", dc, 3);
  if (isnan(t)) return false;
  R.outT10 = (int16_t)lroundf(t * 10); R.outCode = isnan(c) ? -1 : (int8_t)c;
  for (int i = 0; i < 3; i++) { R.dMax10[i] = isnan(mx[i]) ? -999 : (int16_t)lroundf(mx[i] * 10); R.dMin10[i] = isnan(mn[i]) ? -999 : (int16_t)lroundf(mn[i] * 10); R.dCode[i] = isnan(dc[i]) ? -1 : (int8_t)dc[i]; }
  R.wxAt = nowS();
  FL("fetch", "api.open-meteo.com: forecast in " + String((millis() - t0) / 1000.0f, 1) + " s, " + String(b.length()) + " bytes");
  return true;
}
// once an hour with a known network, and right after a cold start: time and weather
static void goOnline(const char *why) {
  if (R.batPct >= 0 && R.batPct < 8 && !R.usb) return;          // a nearly empty cell keeps its charge for the clock
  if (R.wifiN < 255) R.wifiN++;                                // v0.3.0: counted for the 30-minute log
  FL("wifi", String("going online: ") + why);
  bool ok = wifiConnect(8000);
  if (ok) {
    if (!R.ntpAt || nowS() - R.ntpAt > 6 * 3600 || !timeValid()) syncTime();
    if (fetchWeather()) R.fetchFails = 0; else R.fetchFails++;
  } else R.fetchFails++;
  R.lastFetch = nowS();
  wifiOff();
}
// ---- the setup page: hotspot, networks, clock from the phone, language, field log
static String htmlEsc(const String &s) { String o; for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c; } return o; }
static String tzFromPhone(const String &z, int offMin) {
  static const char *const CET[] = { "Europe/Berlin", "Europe/Paris", "Europe/Brussels", "Europe/Amsterdam", "Europe/Luxembourg",
    "Europe/Vienna", "Europe/Zurich", "Europe/Rome", "Europe/Madrid", "Europe/Prague", "Europe/Warsaw", "Europe/Copenhagen",
    "Europe/Stockholm", "Europe/Oslo", "Europe/Budapest", "Europe/Monaco", "Europe/Ljubljana", "Europe/Zagreb", NULL };
  static const char *const WET[] = { "Europe/London", "Europe/Dublin", "Europe/Lisbon", "Atlantic/Canary", NULL };
  static const char *const EET[] = { "Europe/Helsinki", "Europe/Athens", "Europe/Kyiv", "Europe/Kiev", "Europe/Riga", "Europe/Tallinn",
    "Europe/Vilnius", "Europe/Sofia", "Europe/Bucharest", NULL };
  for (int i = 0; CET[i]; i++) if (z == CET[i]) return EK_TZ_DEFAULT;
  for (int i = 0; WET[i]; i++) if (z == WET[i]) return "GMT0BST,M3.5.0/1,M10.5.0";
  for (int i = 0; EET[i]; i++) if (z == EET[i]) return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  if (z == "Europe/Moscow") return "MSK-3";
  if (z == "Asia/Tokyo") return "JST-9";
  if (z == "UTC" || z == "Etc/UTC") return "UTC0";
  if (z == "America/New_York" || z == "America/Toronto" || z == "America/Montreal") return "EST5EDT,M3.2.0,M11.1.0";
  if (offMin < -14 * 60 || offMin > 12 * 60) return tzCache;
  char b[24]; int a = abs(offMin);
  if (a % 60) snprintf(b, sizeof b, "LT%c%d:%02d", offMin < 0 ? '-' : '+', a / 60, a % 60);
  else snprintf(b, sizeof b, "LT%c%d", offMin < 0 ? '-' : '+', a / 60);
  return String(b);
}
static void setTz(const String &r) { tzCache = r; prefs.putString(EK_SET_TZ, r); setenv("TZ", tzCache.c_str(), 1); tzset(); }
static String logText() {
  flFlush(); fsMount();
  String s;
  for (const char *p : { "/field.old", "/field.txt" }) { File f = LittleFS.open(p, "r"); if (f) { s += f.readString(); f.close(); } }
  return s;
}
static void setupPage() {
  R.scr = SCR_SETUP; setupNote = "";
  String pw = apPass();
  WiFi.mode(WIFI_AP); WiFi.softAP(apName().c_str(), pw.c_str());
  delay(200);
  DNSServer dns; dns.start(53, "*", WiFi.softAPIP());
  WebServer srv(80);
  FL("setup", "setup page open");
  uint32_t last = millis(); bool redraw = true, done = false; String note;
  auto page = [&]() {
    nets.load(*kv); R.nets = nets.count;
    String h = F("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>E!nkster 397</title>"
                 "<style>body{font:19px system-ui,sans-serif;max-width:640px;margin:0 auto;padding:16px;background:#f4f1ea;color:#111}"
                 "h1{font-size:26px}h2{font-size:22px;margin-top:28px}input,button,select{font:inherit;padding:10px;margin:4px 0;width:100%;box-sizing:border-box}"
                 ".row{display:flex;gap:8px;align-items:center}.row span{flex:1}.row button{width:auto}.note{background:#fff;border:2px solid #111;padding:10px}"
                 "a{color:#111}small{font-size:15px}</style></head><body>");
    h += "<h1>E!nkster 397 &middot; " EK_VERSION "</h1>";
    if (note.length()) h += "<p class=note>" + htmlEsc(note) + "</p>";
    h += "<h2>" + String(L("Wi-Fi networks", "Réseaux Wi-Fi")) + "</h2>";
    for (int i = 0; i < nets.count; i++)
      h += "<form method=post action=/del class=row><span>" + htmlEsc(nets.name[i]) + "</span><input type=hidden name=i value=" + i + "><button>" + L("Remove", "Retirer") + "</button></form>";
    if (!nets.count) h += "<p>" + String(L("None yet.", "Aucun pour l'instant.")) + "</p>";
    h += "<form method=post action=/save><input name=s placeholder='" + String(L("Network name", "Nom du réseau")) + "' required><input name=p type=password placeholder='" + L("Password", "Mot de passe") +
         "'><button>" + L("Add or update", "Ajouter ou mettre à jour") + "</button></form>";
    h += "<h2>" + String(L("Clock", "Horloge")) + "</h2><form method=post action=/time id=t><input type=hidden name=e><input type=hidden name=z><input type=hidden name=o><button>" +
         L("Set the clock from this phone", "Régler l'horloge sur ce téléphone") + "</button></form>"
         "<script>document.getElementById('t').onsubmit=function(){this.e.value=Math.floor(Date.now()/1000);this.z.value=Intl.DateTimeFormat().resolvedOptions().timeZone||'';this.o.value=new Date().getTimezoneOffset();}</script>";
    h += "<h2>" + String(L("Language", "Langue")) + "</h2><form method=post action=/lang class=row><button name=l value=en>English</button><button name=l value=fr>Français</button></form>";
    h += "<h2>" + String(L("Field log", "Journal de terrain")) + "</h2><p><a href=/log>" + L("Read it", "Le lire") + "</a> &middot; <a href=/log?dl=1>" + L("Download it", "Le télécharger") + "</a></p>";
    h += "<h2>" + String(L("30-minute log", "Journal de 30 minutes")) + "</h2><p><a href=/log.csv>" + L("Download it as a table (CSV)", "Le télécharger en tableau (CSV)") + "</a><br><small>" +
         L("Battery, room and charge every 30 minutes, the same columns as the 154 (the fifth is the battery percentage).", "Batterie, pièce et charge toutes les 30 minutes, les colonnes de la 154 (la cinquième est le pourcentage de batterie).") + "</small></p>";
    h += "<form method=post action=/close><button>" + String(L("Close and go back to the tiles", "Fermer et revenir aux tuiles")) + "</button></form>";
    h += "<p><small>" + String(L("No key, network name or password ever goes into the field log.", "Aucune clé, aucun nom de réseau ni mot de passe ne va dans le journal.")) + "</small></p></body></html>";
    srv.send(200, "text/html; charset=utf-8", h);
  };
  srv.on("/", HTTP_GET, [&]() { last = millis(); page(); });
  srv.on("/save", HTTP_POST, [&]() {
    last = millis(); nets.load(*kv);
    if (nets.add(*kv, srv.arg("s").c_str(), srv.arg("p").c_str())) { note = L("Network saved.", "Réseau enregistré."); FL("setup", "a network saved, " + String(nets.count) + " known"); }
    else note = L("Name or password too long.", "Nom ou mot de passe trop long.");
    R.nets = nets.count; page();
  });
  srv.on("/del", HTTP_POST, [&]() { last = millis(); nets.load(*kv); if (nets.removeAt(*kv, srv.arg("i").toInt())) { note = L("Network removed.", "Réseau retiré."); FL("setup", "a network removed, " + String(nets.count) + " known"); } R.nets = nets.count; page(); });
  srv.on("/time", HTTP_POST, [&]() {
    last = millis(); long e = srv.arg("e").toInt();
    if (e > 1735689600) {
      struct timeval tv = { (time_t)e, 0 }; settimeofday(&tv, NULL);
      setTz(tzFromPhone(srv.arg("z"), srv.arg("o").toInt())); systemToRtcChip();
      note = L("Clock set.", "Horloge réglée."); FL("time", "clock set from the phone");
    } else note = L("The phone sent no time.", "Le téléphone n'a pas envoyé l'heure.");
    page();
  });
  srv.on("/lang", HTTP_POST, [&]() {
    last = millis(); String l = srv.arg("l");
    if (l == "en" || l == "fr") { prefs.putString(EK_SET_LANG, l); uiFr = R.fr = (l == "fr"); note = L("Language saved: English.", "Langue enregistrée : français."); FL("setup", "interface language set to " + l); redraw = true; }
    page();
  });
  srv.on("/log", HTTP_GET, [&]() {
    last = millis();
    if (srv.hasArg("dl")) srv.sendHeader("Content-Disposition", "attachment; filename=einkster397-field-log.txt");
    srv.send(200, "text/plain; charset=utf-8", logText());
  });
  srv.on("/log.csv", HTTP_GET, [&]() {            // v0.3.0
    last = millis();
    uint8_t *b = NULL; size_t sz = fsRead("/log.bin", &b); int n = sz / sizeof(BatSample);
    srv.sendHeader("Content-Disposition", "attachment; filename=einkster397-30min-log.csv");
    srv.setContentLength(CONTENT_LENGTH_UNKNOWN); srv.send(200, "text/csv; charset=utf-8", "");
    String chunk = LOG_CSV_HEAD; char line[160];
    for (int i = 0; i < n; i++) { BatSample s; memcpy(&s, b + i * sizeof s, sizeof s); logCsvLine(s, line, sizeof line); chunk += line; if (chunk.length() > 3000) { srv.sendContent(chunk); chunk = ""; } }
    if (chunk.length()) srv.sendContent(chunk);
    srv.sendContent("");
    if (b) free(b);
  });
  srv.on("/close", HTTP_POST, [&]() { srv.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><p style='font:20px system-ui'>" + String(L("Closed. You can leave this hotspot.", "Fermé. Vous pouvez quitter ce point d'accès.")) + "</p>"); done = true; });
  srv.onNotFound([&]() { srv.sendHeader("Location", "http://192.168.4.1/"); srv.send(302, "text/plain", ""); });
  srv.begin();
  while (!done && millis() - last < 5 * 60 * 1000UL) {
    dns.processNextRequest(); srv.handleClient();
    if (redraw) { redraw = false; freshen(); render(); push(false); }
    if (btn(PIN_PRESS)) { uint32_t t0 = millis(); while (btn(PIN_PRESS) && millis() - t0 < 800) delay(10); if (millis() - t0 >= 700) { while (btn(PIN_PRESS)) delay(10); break; } }
    { keySink = escSink; TwRep r; while (kbPoll(r)) twReport(r); keySink = NULL; if (navEsc) { navEsc = false; break; } }   // v0.4.0: Esc closes
    delay(5);
  }
  srv.stop(); dns.stop(); WiFi.softAPdisconnect(true); wifiOff();
  FL("setup", done ? "setup page closed from the phone" : "setup page closed");
  R.scr = SCR_TILE; R.tile = T_DEVICE;
}
#else
static void goOnline(const char *) { R.lastFetch = nowS(); if (R.wifiN < 255) R.wifiN++; }
static void setupPage() { R.scr = SCR_SETUP; render(); push(false); R.scr = SCR_TILE; R.tile = T_DEVICE; }
static void setTz(const String &r) { tzCache = r; prefs.putString(EK_SET_TZ, r); setenv("TZ", tzCache.c_str(), 1); tzset(); }
#endif

// ================================================================ the dial
#define AWAKE_MS 45000
#define LONG_MS 700
enum { EV_NONE, EV_UP, EV_DOWN, EV_PRESS, EV_LONG };
static uint32_t lastInput = 0;
static int readDial() {                         // one event, or none; waits for a press to end
  for (int p : { PIN_UP, PIN_DOWN }) if (btn(p)) {
    uint32_t t0 = millis(); while (btn(p) && millis() - t0 < 400) delay(8);
    lastInput = millis(); return p == PIN_UP ? EV_UP : EV_DOWN;   // still held: the next read repeats it
  }
  if (btn(PIN_PRESS)) {
    uint32_t t0 = millis(); while (btn(PIN_PRESS) && millis() - t0 < LONG_MS) delay(8);
    bool lng = millis() - t0 >= LONG_MS;
    uint32_t t1 = millis(); while (btn(PIN_PRESS) && millis() - t1 < 5000) delay(8);
    lastInput = millis(); return lng ? EV_LONG : EV_PRESS;
  }
  return EV_NONE;
}
static void openList(int l) { R.scr = SCR_LIST; R.list = l; R.cur = 0; R.top = 0; }
static void switchOff(const char *why) {
  FL("end", String("switched off: ") + why); flFlush();
  brand("OFF"); push(true);
  delay(200); pmuOff();
  sleepFor(0);                                  // on USB the power chip may keep it alive: sleep until the dial
}
static void goSleep();
static bool twPairFlow(); static void shKbStart();
static void shAct(int ev); static void shEnter(); static void shOpenArc(int i); static void navKey(uint8_t mod, uint8_t u);
// ================================================================ the typewriter (v0.2.0)
// A Bluetooth Low Energy keyboard (pairs once, kept bonded), the page turned to 800 x 480, and a forward-only writer:
// type a line, Enter appends it to /typed/<date>.txt on the microSD. Saved lines are never edited; Backspace works only
// in the line being typed. Only the rows that changed are refreshed; a clean full refresh comes every 40 of them.
// Keys never wait for the paper: they are queued as they arrive and the paper catches up with all of them at once.
// The journal keeps one line per writing session (its length, lines saved, key-to-paper times), never the words.
#define TW_IDLE_MS (10UL * 60 * 1000)          // ten minutes without a key or the dial: the session ends, it sleeps
#define TW_FULL_EVERY 40                       // window refreshes before a clean full one (at the next Enter)
#define TW_SHOW 11                             // saved rows shown above the line being typed
static String twShown[TW_SHOW]; static int twShownN = 0;   // the last saved lines, oldest first (for the page only)
static int twToday = 0;                        // lines in today's file
static bool twCard = false; static String twNote;
static uint32_t twKeyAt = 0;                   // arrival of the oldest key not yet on the paper (0: none waiting)
static uint32_t twLat[64]; static int twLatN = 0; static uint32_t twLastLat = 0;
static uint32_t twShownLat = 0;              // the figure in the foot changes only when the page is drawn clean, so the foot stays out of the key windows
static uint16_t twWindows = 0;                 // window refreshes since the last full one
static uint32_t twLastInput = 0, twSessionAt = 0, twSessionLines = 0, twSessionKeys = 0;
static uint8_t twPrevKeys[6];
static std::vector<uint16_t> twKeyHandles;

// ---- the page turned: 0 upright (the tiles), 1 landscape, 2 landscape the other way round
static void setRot(uint8_t r) { rot = r; if (r == 0) { W = 480; H = 800; } else { W = 800; H = 480; } }
static void twUpright() { if (rot) { setRot(0); havePrev = false; } }

// ---- the German layout (DIN 2137 T1, as a PC reads it): HID usage -> normal, Shift, AltGr. UTF-8. "" = nothing.
static const KeyDe DE[] = {
  { 0x04, "a", "A", "" }, { 0x05, "b", "B", "" }, { 0x06, "c", "C", "" }, { 0x07, "d", "D", "" },
  { 0x08, "e", "E", "\xE2\x82\xAC" }, { 0x09, "f", "F", "" }, { 0x0A, "g", "G", "" }, { 0x0B, "h", "H", "" },
  { 0x0C, "i", "I", "" }, { 0x0D, "j", "J", "" }, { 0x0E, "k", "K", "" }, { 0x0F, "l", "L", "" },
  { 0x10, "m", "M", "\xC2\xB5" }, { 0x11, "n", "N", "" }, { 0x12, "o", "O", "" }, { 0x13, "p", "P", "" },
  { 0x14, "q", "Q", "@" }, { 0x15, "r", "R", "" }, { 0x16, "s", "S", "" }, { 0x17, "t", "T", "" },
  { 0x18, "u", "U", "" }, { 0x19, "v", "V", "" }, { 0x1A, "w", "W", "" }, { 0x1B, "x", "X", "" },
  { 0x1C, "z", "Z", "" }, { 0x1D, "y", "Y", "" },
  { 0x1E, "1", "!", "" }, { 0x1F, "2", "\"", "\xC2\xB2" }, { 0x20, "3", "\xC2\xA7", "\xC2\xB3" }, { 0x21, "4", "$", "" },
  { 0x22, "5", "%", "" }, { 0x23, "6", "&", "" }, { 0x24, "7", "/", "{" }, { 0x25, "8", "(", "[" },
  { 0x26, "9", ")", "]" }, { 0x27, "0", "=", "}" },
  { 0x2C, " ", " ", " " }, { 0x2B, "    ", "    ", "    " },
  { 0x2D, "\xC3\x9F", "?", "\\" },
  { 0x2F, "\xC3\xBC", "\xC3\x9C", "" }, { 0x30, "+", "*", "~" },
  { 0x31, "#", "'", "" }, { 0x32, "#", "'", "" },
  { 0x33, "\xC3\xB6", "\xC3\x96", "" }, { 0x34, "\xC3\xA4", "\xC3\x84", "" },
  { 0x36, ",", ";", "" }, { 0x37, ".", ":", "" }, { 0x38, "-", "_", "" },
  { 0x64, "<", ">", "|" },
  // the numeric keypad, if the keyboard has one
  { 0x54, "/", "/", "" }, { 0x55, "*", "*", "" }, { 0x56, "-", "-", "" }, { 0x57, "+", "+", "" },
  { 0x59, "1", "1", "" }, { 0x5A, "2", "2", "" }, { 0x5B, "3", "3", "" }, { 0x5C, "4", "4", "" }, { 0x5D, "5", "5", "" },
  { 0x5E, "6", "6", "" }, { 0x5F, "7", "7", "" }, { 0x60, "8", "8", "" }, { 0x61, "9", "9", "" }, { 0x62, "0", "0", "" }, { 0x63, ",", ",", "" },
};
// the dead keys: ^ (0x35), ´ (0x2E), ` (Shift 0x2E). They wait for the next key and combine with a vowel.
static const char *deadChar(uint8_t d) { return d == 1 ? "^" : d == 2 ? "\xC2\xB4" : "`"; }
static const char *combine(uint8_t d, char v, bool up) {
  static const char *CIRC[] = { "\xC3\xA2", "\xC3\xAA", "\xC3\xAE", "\xC3\xB4", "\xC3\xBB", "\xC3\x82", "\xC3\x8A", "\xC3\x8E", "\xC3\x94", "\xC3\x9B" };
  static const char *ACUT[] = { "\xC3\xA1", "\xC3\xA9", "\xC3\xAD", "\xC3\xB3", "\xC3\xBA", "\xC3\x81", "\xC3\x89", "\xC3\x8D", "\xC3\x93", "\xC3\x9A" };
  static const char *GRAV[] = { "\xC3\xA0", "\xC3\xA8", "\xC3\xAC", "\xC3\xB2", "\xC3\xB9", "\xC3\x80", "\xC3\x88", "\xC3\x8C", "\xC3\x92", "\xC3\x99" };
  const char *vow = "aeiou"; const char *p = strchr(vow, v); if (!p || !v) return NULL;
  int i = (p - vow) + (up ? 5 : 0);
  return d == 1 ? CIRC[i] : d == 2 ? ACUT[i] : GRAV[i];
}
static void twAppend(const char *s) {
  size_t n = strlen(R.tline), k = strlen(s);
  if (n + k + 1 < sizeof R.tline) { memcpy(R.tline + n, s, k + 1); twSessionKeys++; }
}
static void twBackspace() {
  int n = strlen(R.tline); if (!n) return;
  int c = n - 1; while (c > 0 && ((uint8_t)R.tline[c] & 0xC0) == 0x80) c--;
  R.tline[c] = 0;
}
// the text a key writes in the German layer, dead keys and Caps Lock included ("" for none). v0.3.0: shared.
static String deText(uint8_t mod, uint8_t u) {
  bool shift = mod & 0x22, altgr = (mod & 0x40) || ((mod & 0x11) && (mod & 0x04));
  uint8_t dead = 0;
  if (u == 0x35 && !shift && !altgr) dead = 1;                       // ^
  if (u == 0x2E && !altgr) dead = shift ? 3 : 2;                     // ` (Shift) or ´
  if (dead) { String r = R.dead ? deadChar(R.dead) : ""; R.dead = dead; return r; }
  if (u == 0x35 && shift) return "\xC2\xB0";                          // °
  const char *out = NULL; bool letter = (u >= 0x04 && u <= 0x1D) || u == 0x2F || u == 0x33 || u == 0x34;
  for (const KeyDe &e : DE) if (e.u == u) { bool up = letter && !altgr ? (shift != (bool)R.caps) : shift; out = altgr ? e.a : up ? e.s : e.n; break; }
  if (!out || !out[0]) { String r = R.dead ? deadChar(R.dead) : ""; R.dead = 0; return r; }   // arrows, F-keys: nothing to write
  if (R.dead) {
    uint8_t d = R.dead; R.dead = 0;
    bool up = out[0] >= 'A' && out[0] <= 'Z';
    const char *c = out[1] ? NULL : combine(d, (char)tolower(out[0]), up);
    if (c) return c;
    if (u == 0x2C) return deadChar(d);                               // dead key then space: the accent alone
    return String(deadChar(d)) + out;
  }
  return out;
}
static void twSave();
static bool twEscLeave = false;
static bool twCleanNext = false;              // v0.2.1: Enter is a carriage return, the whole page drawn clean (window refreshes leave grey where text is erased)
// one key that went down: modifiers as the keyboard sent them
static void twKey(uint8_t mod, uint8_t u) {
  bool altgr = (mod & 0x40) || ((mod & 0x11) && (mod & 0x04)), ctrl = (mod & 0x11) && !altgr;
  if (u == 0x39) { R.caps = !R.caps; return; }                       // Caps Lock
  if (u == 0x28 || u == 0x58) { if (R.dead) { twAppend(deadChar(R.dead)); R.dead = 0; } twSave(); return; }   // Enter (and keypad Enter)
  if (u == 0x2A) { if (R.dead) R.dead = 0; else twBackspace(); return; }                                      // Backspace: only this line
  if (u == 0x29) { twEscLeave = true; return; }                      // v0.4.0: Esc leaves, like the long press
  if (ctrl) return;                                                  // no shortcuts in a forward-only writer
  String t = deText(mod, u);                                         // v0.3.0: the letters, shared with the archives' search
  if (t.length()) twAppend(t.c_str());
}
// one report from the keyboard: the keys that are new in it
static void twReport(const TwRep &r) {
  bool isKey = false; for (uint16_t h : twKeyHandles) if (h == r.handle) isKey = true;
  if (!isKey) return;
  const uint8_t *d = r.d; int n = r.n;
  if (n == 9) { d++; n = 8; }
  if (n != 8) return;
  const uint8_t *k = d + 2; bool any = false;
  for (int i = 0; i < 6; i++) {
    uint8_t u = k[i]; if (u < 4) continue;
    bool was = false; for (int j = 0; j < 6; j++) if (twPrevKeys[j] == u) was = true;
    if (!was) { if (keySink) keySink(d[0], u); else twKey(d[0], u); any = true; }
  }
  memcpy(twPrevKeys, k, 6);
  if (any) { if (!twKeyAt) twKeyAt = r.ms; twLastInput = millis(); }
}

// ---- the microSD (4-bit: CLK 16, CMD 17, D0 15, D1 7, D2 8, D3 18; Waveshare's demo)
static String twPath() {
  if (!timeValid()) return "/typed/undated.txt";
  time_t e = time(NULL); struct tm t; localtime_r(&e, &t); char b[32]; strftime(b, sizeof b, "/typed/%Y-%m-%d.txt", &t); return String(b);
}
#ifndef EK_HOST
static bool sdUp = false;
static bool sdBegin() {
  if (sdUp) return true;
  SD_MMC.setPins(16, 17, 15, 7, 8, 18);
  sdUp = SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5);
  if (!sdUp) { SD_MMC.setPins(16, 17, 15); sdUp = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5); }   // one data line
  if (sdUp && !SD_MMC.exists("/typed")) SD_MMC.mkdir("/typed");
  return sdUp;
}
static void sdEnd() { if (sdUp) SD_MMC.end(); sdUp = false; }
static long sdSize(const String &p) { File f = SD_MMC.open(p, "r"); if (!f) return -1; long n = f.size(); f.close(); return n; }
static bool sdAppend(const String &p, const char *s, size_t n) { File f = SD_MMC.open(p, FILE_APPEND); if (!f) return false; size_t w = f.write((const uint8_t *)s, n); f.close(); return w == n; }
static String sdRead(const String &p, long from) { File f = SD_MMC.open(p, "r"); if (!f) return ""; if (from > 0) f.seek(from); String s = f.readString(); f.close(); return s; }
#else
static bool sdUp = false;
static String hostSd(const String &p) { return String(hostFs.c_str()) + "/sd" + p; }
static bool sdBegin() { if (!hw.sdThere) return false; sdUp = true; std::string d = hostFs + "/sd/typed"; std::string c = "mkdir -p '" + d + "'"; if (system(c.c_str())) {} return true; }
static void sdEnd() { sdUp = false; }
static long sdSize(const String &p) { FILE *f = fopen(hostSd(p).c_str(), "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
static bool sdAppend(const String &p, const char *s, size_t n) { if (!hw.sdThere) return false; FILE *f = fopen(hostSd(p).c_str(), "ab"); if (!f) return false; size_t w = fwrite(s, 1, n, f); fclose(f); return w == n; }
static String sdRead(const String &p, long from) { FILE *f = fopen(hostSd(p).c_str(), "rb"); if (!f) return ""; if (from > 0) fseek(f, from, SEEK_SET); String s; char b[512]; size_t k; while ((k = fread(b, 1, sizeof b - 1, f)) > 0) { b[k] = 0; s += b; } fclose(f); return s; }
#endif
// today's file: how many lines, and the last ones for the page
static void twLoadToday() {
  twShownN = 0; twToday = 0;
  if (!twCard) return;
  String p = twPath(); long n = sdSize(p); if (n <= 0) return;
  String all = sdRead(p, n > 6000 ? n - 6000 : 0);
  for (unsigned i = 0; i < all.length(); i++) if (all[i] == '\n') twToday++;
  if (n > 6000) { // the count needs the whole file
    twToday = 0; long at = 0; while (at < n) { String part = sdRead(p, at); if (!part.length()) break; for (unsigned i = 0; i < part.length(); i++) if (part[i] == '\n') twToday++; at += part.length(); if (part.length() < 64) break; }
  }
  int end = all.length();
  while (twShownN < TW_SHOW && end > 0) {
    int e = end - 1; if (e >= 0 && all[e] == '\n') e--;
    int s = e; while (s >= 0 && all[s] != '\n') s--;
    if (e < 0) break;
    for (int i = twShownN; i > 0; i--) twShown[i] = twShown[i - 1];
    twShown[0] = all.substring(s + 1, e + 1); twShownN++;
    end = s + 1; if (s < 0) break;
  }
}
// Enter: the line goes to the card, is read back, and moves up the page
static void twSave() {
  String line = R.tline;
  bool ok = false;
  if (twCard || (twCard = sdBegin())) {
    String p = twPath(); long before = sdSize(p); if (before < 0) before = 0;
    String w = line + "\n";
    ok = sdAppend(p, w.c_str(), w.length()) && sdSize(p) == before + (long)w.length() && sdRead(p, before) == w;   // written and read back
    if (!ok) { twCard = false; sdEnd(); }
  }
  if (ok) { twToday++; twSessionLines++; twNote = ""; }
  else twNote = L("NO CARD: this line was not saved", "PAS DE CARTE\xC2\xA0: ligne non enregistr\xC3\xA9" "e");
  if (twShownN == TW_SHOW) { for (int i = 0; i + 1 < TW_SHOW; i++) twShown[i] = twShown[i + 1]; twShownN--; }
  twShown[twShownN++] = ok ? line : String("\xE2\x9C\x97 ") + line;
  R.tline[0] = 0; twCleanNext = true;
}

// ---- the page, 800 x 480
#define TW_LINE_Y 384                           // the line being typed
static uint32_t twMedian() {
  if (!twLatN) return 0;
  uint32_t a[64]; int n = min(twLatN, 64); memcpy(a, twLat, n * sizeof(uint32_t)); std::sort(a, a + n); return a[n / 2];
}
static void drawType() {
  // the top: date, lines today, keyboard, card, the last key-to-paper time
  String top = String(L("Typewriter", "Machine \xC3\xA0 \xC3\xA9" "crire"));
  if (timeValid()) { char b[40]; dateLine(b, sizeof b, time(NULL)); top += "  \xC2\xB7  " + String(b); }
  top += "  \xC2\xB7  " + String(twToday) + L(" lines today", " lignes aujourd'hui");
  textLeft(EK_SMALL, 16, 6, top, true, W - 260);
  String right = (R.kbBat >= 0 ? String(L("keyboard ", "clavier ")) + (int)R.kbBat + " %" : String(""));
  right += String(right.length() ? "  \xC2\xB7  " : "") + (twCard ? L("card", "carte") : L("NO CARD", "PAS DE CARTE"));
  textRight(EK_SMALL, W - 16, 6, right);
  rect(12, 30, W - 24, 2, true);
  // the saved lines, newest at the bottom, wrapped
  static int st[40], en[40];
  int y = TW_LINE_Y - 22, rows = 0;
  for (int i = twShownN - 1; i >= 0 && rows < 12; i--) {
    int n = decodeUtf8(twShown[i].c_str(), tbuf, 1600);
    int k = wrap(EK_MID, tbuf, n, W - 48, st, en, 40); if (k == 0) k = 1, st[0] = en[0] = 0;
    for (int l = k - 1; l >= 0 && rows < 12; l--) {
      y -= EK_MID.height + 4; if (y < 38) break;
      int x = 24; for (int c = st[l]; c < en[l]; c++) { glyph(EK_MID, x, y, tbuf[c], true); x += glyphW(EK_MID, tbuf[c]); }
      rows++;
    }
    if (y < 38) break;
  }
  rect(12, TW_LINE_Y - 12, W - 24, 2, true);
  // the line being typed: its end, with the cursor
  String cur = String(R.tline) + (R.dead ? deadChar(R.dead) : "");
  int maxW = W - 64, w = textWidth(EK_BIG, cur), x = 24;
  if (w > maxW) {                                                    // a long line: its end stays in view
    int n = decodeUtf8(cur.c_str(), tbuf, 1600), s = n, ww = 0;
    while (s > 0 && ww + glyphW(EK_BIG, tbuf[s - 1]) < maxW - 30) ww += glyphW(EK_BIG, tbuf[--s]);
    glyph(EK_BIG, x, TW_LINE_Y, 0x2026, true); x += glyphW(EK_BIG, 0x2026);
    for (int c = s; c < n; c++) { glyph(EK_BIG, x, TW_LINE_Y, tbuf[c], true); x += glyphW(EK_BIG, tbuf[c]); }
  } else { textLeft(EK_BIG, x, TW_LINE_Y, cur); x += w; }
  rect(x + 2, TW_LINE_Y + 2, 4, EK_BIG.height - 4, true);           // the cursor
  if (R.caps) textRight(EK_SMALL, W - 16, TW_LINE_Y + 8, "CAPS");
  // the foot
  String foot = twNote.length() ? twNote : String(L("Enter: save  \xC2\xB7  Backspace: this line  \xC2\xB7  long press: leave",
                                                     "Entr\xC3\xA9" "e\xC2\xA0: enregistrer  \xC2\xB7  Retour\xC2\xA0: cette ligne  \xC2\xB7  appui long\xC2\xA0: quitter"));
  if (!twNote.length() && twShownLat) foot += String("  \xC2\xB7  ") + L("key to paper ", "touche \xE2\x86\x92 papier ") + twShownLat + " ms";
  textLeft(EK_SMALL, 16, H - 26, foot, true, W - 32);
}

// ---- refresh only the rows that changed
static int twWindowRows = 0;
static void twPushChanged(bool full) {
  if (full || !havePrev || !prevFb) { push(true); epdHot = false; twWindows = 0; return; }
  int r0 = -1, r1 = -1;
  for (int r = 0; r < NH; r++) if (memcmp(fb + r * (NW / 8), prevFb + r * (NW / 8), NW / 8)) { if (r0 < 0) r0 = r; r1 = r; }
  if (r0 < 0) return;
  twWindowRows = r1 - r0 + 1;
  epdPushRows(r0, r1, prevFb);
  memcpy(prevFb + r0 * (NW / 8), fb + r0 * (NW / 8), (r1 - r0 + 1) * (NW / 8));
  twWindows++;
}
// the paper catches up with every key that arrived; the time from the oldest key to the end of the refresh is measured
static void twPaper(bool full) {
  uint32_t keyAt = twKeyAt; twKeyAt = 0;
  if (full && twLatN) twShownLat = twMedian();
  clearFb(); drawType(); twPushChanged(full);
  if (keyAt && !full) { twLastLat = millis() - keyAt; twLat[twLatN % 64] = twLastLat; twLatN++; }
}

// ---- the keyboard (Bluetooth Low Energy, the Arduino core's BLE library on Bluedroid; proven by kbtest 0.3)
static std::vector<KbFound> kbFound; static int kbOthers = 0, kbCur = 0;
static volatile uint32_t kbPass = 0; static volatile bool kbAuthDone = false, kbAuthOk = false, kbGone = false;
static volatile int kbBatNew = -1;
static QueueHandle_t twQ = NULL;
static String kbMsg;
static bool kbSaved(uint8_t *a, uint8_t &type) { if (prefs.getBytesLength("kbA") != 6) return false; prefs.getBytes("kbA", a, 6); type = prefs.getUChar("kbT", 0); return true; }
static void kbForget() { prefs.remove("kbA"); prefs.remove("kbT"); R.kbBat = -1; FL("keyboard", "forgotten"); }
#ifndef EK_HOST
static BLEClient *kbClient = NULL;
class KbSec : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t k) override { kbPass = k; }
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t c) override { kbAuthOk = c.success; kbAuthDone = true; }
};
class KbCl : public BLEClientCallbacks { void onConnect(BLEClient *) override {} void onDisconnect(BLEClient *) override { kbGone = true; } };
static KbSec kbSec; static KbCl kbCl;
static void kbNoop(BLERemoteCharacteristic *, uint8_t *, size_t, bool) {}
static uint16_t kbBatHandle = 0;
static void kbHook(esp_gattc_cb_event_t e, esp_gatt_if_t, esp_ble_gattc_cb_param_t *p) {   // every notification, straight from Bluedroid
  if (e != ESP_GATTC_NOTIFY_EVT || !twQ) return;
  if (p->notify.handle == kbBatHandle && p->notify.value_len) { kbBatNew = p->notify.value[0]; return; }
  TwRep r; r.n = p->notify.value_len > sizeof r.d ? sizeof r.d : p->notify.value_len; memcpy(r.d, p->notify.value, r.n);
  r.handle = p->notify.handle; r.ms = millis();
  xQueueSend(twQ, &r, 0);
}
static void kbInit() {
  if (BLEDevice::getInitialized()) return;
  BLEDevice::init("");                                     // no name of its own: the 397 only listens
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(&kbSec);
  BLEDevice::setCustomGattcHandler(kbHook);
  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_OUT);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
}
static void kbOff() { if (kbClient && kbClient->isConnected()) kbClient->disconnect(); }
static bool kbConnected() { return kbClient && kbClient->isConnected() && !kbGone; }
static bool kbIsKeyboard(BLEAdvertisedDevice &d) { uint16_t app = d.haveAppearance() ? d.getAppearance() : 0; return d.isAdvertisingService(BLEUUID((uint16_t)0x1812)) || (app >= 0x03C0 && app <= 0x03FF); }
static void kbScan() {
  kbInit(); kbFound.clear(); kbOthers = 0; kbCur = 0;
  BLEScan *s = BLEDevice::getScan(); s->setActiveScan(true); s->setInterval(100); s->setWindow(99);
  BLEScanResults *r = s->start(6, false);
  for (int i = 0; r && i < r->getCount(); i++) {
    BLEAdvertisedDevice d = r->getDevice(i);
    if (!kbIsKeyboard(d)) { kbOthers++; continue; }
    KbFound f = {}; memcpy(f.a, *d.getAddress().getNative(), 6); f.type = d.getAddressType(); f.rssi = d.getRSSI(); f.app = d.haveAppearance() ? d.getAppearance() : 0;
    if (d.haveName()) snprintf(f.name, sizeof f.name, "%s", d.getName().c_str());
    kbFound.push_back(f);
  }
  s->clearResults();
  std::sort(kbFound.begin(), kbFound.end(), [](const KbFound &a, const KbFound &b) { return a.rssi > b.rssi; });
}
static bool kbSubscribe() {
  BLERemoteService *hid = kbClient->getService(BLEUUID((uint16_t)0x1812));
  if (!hid) return false;
  twKeyHandles.clear();
  for (auto &kv : *hid->getCharacteristicsByHandle()) {     // report mode, every channel that notifies (kbtest 0.3)
    BLERemoteCharacteristic *c = kv.second;
    if (!(c->canNotify() || c->canIndicate())) continue;
    c->registerForNotify(kbNoop, c->canNotify()); twKeyHandles.push_back(c->getHandle());
  }
  BLERemoteService *bat = kbClient->getService(BLEUUID((uint16_t)0x180F));
  kbBatHandle = 0;
  if (bat) { BLERemoteCharacteristic *c = bat->getCharacteristic(BLEUUID((uint16_t)0x2A19));
    if (c) { kbBatHandle = c->getHandle(); if (c->canRead()) { String v = c->readValue(); if (v.length()) R.kbBat = (uint8_t)v[0]; } if (c->canNotify()) c->registerForNotify(kbNoop); } }
  return !twKeyHandles.empty();
}
static void drawKb();
static bool kbConnect(const uint8_t *a, uint8_t type, uint32_t timeoutMs, bool pairing) {
  kbInit();
  if (!kbClient) { kbClient = BLEDevice::createClient(); kbClient->setClientCallbacks(&kbCl); }
  kbAuthDone = false; kbAuthOk = false; kbGone = false; kbPass = 0;
  esp_bd_addr_t bda; memcpy(bda, a, 6);
  if (!kbClient->connect(BLEAddress(bda), (esp_ble_addr_type_t)type, timeoutMs)) return false;
  uint32_t t0 = millis(); bool shown = false;
  while (!kbAuthDone && millis() - t0 < 60000 && !kbGone) {
    if (kbPass && !shown) { shown = true; clearFb(); drawKb(); push(false); }
    if (pairing && btn(PIN_PRESS)) { uint32_t h = millis(); while (btn(PIN_PRESS)) delay(10); if (millis() - h > 700) break; }
    delay(20);
  }
  kbPass = 0;
  if (!kbAuthDone || !kbAuthOk || kbGone) { kbOff(); return false; }
  return kbSubscribe();
}
// after a sleep: listen for the keyboard (it may come back from another address) and take it back, no new pairing
static bool kbListen(uint32_t budgetMs) {
  uint8_t a[6], type; if (!kbSaved(a, type)) return false;
  kbInit(); uint32_t t0 = millis();
  while (millis() - t0 < budgetMs) {
    BLEScan *s = BLEDevice::getScan(); s->setActiveScan(true); s->setInterval(60); s->setWindow(59);
    BLEScanResults *r = s->start(2, false);
    int pick = -1, hid = 0, only = -1;
    for (int i = 0; r && i < r->getCount(); i++) { BLEAdvertisedDevice d = r->getDevice(i); if (!memcmp(*d.getAddress().getNative(), a, 6)) pick = i; if (kbIsKeyboard(d)) { hid++; only = i; } }
    if (pick < 0 && hid == 1) pick = only;
    if (pick >= 0) {
      BLEAdvertisedDevice d = r->getDevice(pick); uint8_t na[6]; memcpy(na, *d.getAddress().getNative(), 6); uint8_t nt = d.getAddressType(); s->clearResults();
      if (kbConnect(na, nt, 8000, false)) return true;
    } else s->clearResults();
    if (btn(PIN_PRESS)) return false;                         // the dial: the person wants something else
  }
  return false;
}
static volatile bool kbScanDone = false; static bool kbScanning = false;
static void kbScanCb(BLEScanResults) { kbScanDone = true; }
static void kbListenStop() { if (kbScanning) { BLEDevice::getScan()->stop(); BLEDevice::getScan()->clearResults(); kbScanning = false; } }
static int kbListenStep() {                    // v0.4.0: a one-second round in the background; 1 = the keyboard is back
  uint8_t a[6], type; if (!kbSaved(a, type)) return -1;
  kbInit();
  BLEScan *s = BLEDevice::getScan();
  if (!kbScanning) { s->setActiveScan(true); s->setInterval(60); s->setWindow(59); kbScanDone = false; kbScanning = s->start(1, kbScanCb, false); return 0; }
  if (!kbScanDone) return 0;
  kbScanning = false;
  BLEScanResults *r = s->getResults();
  int pick = -1, hid = 0, only = -1;
  for (int i = 0; r && i < r->getCount(); i++) { BLEAdvertisedDevice d = r->getDevice(i); if (!memcmp(*d.getAddress().getNative(), a, 6)) pick = i; if (kbIsKeyboard(d)) { hid++; only = i; } }
  if (pick < 0 && hid == 1) pick = only;
  if (pick < 0) { s->clearResults(); return 0; }
  BLEAdvertisedDevice d = r->getDevice(pick); uint8_t na[6]; memcpy(na, *d.getAddress().getNative(), 6); uint8_t nt = d.getAddressType(); s->clearResults();
  return kbConnect(na, nt, 8000, false) ? 1 : 0;
}
static bool kbPoll(TwRep &r) { return twQ && xQueueReceive(twQ, &r, 0) == pdTRUE; }
#else
// the bench: a keyboard that is there or not, and keys typed at given times (host.h)
static void kbInit() {}
static void kbOff() { hw.kbConnected = false; }
static bool kbConnected() { return hw.kbConnected; }
static void kbScan() { kbFound.clear(); kbOthers = 1; kbCur = 0; if (hw.kbThere) { KbFound f = {}; f.a[0] = 0xBE; f.type = 1; f.rssi = -55; strcpy(f.name, "bench keyboard"); kbFound.push_back(f); } }
static bool kbConnect(const uint8_t *, uint8_t, uint32_t, bool) { if (!hw.kbThere) return false; hw.kbConnected = true; twKeyHandles.clear(); twKeyHandles.push_back(0x24); R.kbBat = hw.kbBat; return true; }
static bool kbListen(uint32_t) { uint8_t a[6], t; if (!kbSaved(a, t) || !hw.kbThere) return false; return kbConnect(a, t, 0, false); }
static uint32_t kbStepAt = 0;
static void kbListenStop() { kbStepAt = 0; }
static int kbListenStep() { uint8_t a[6], t; if (!kbSaved(a, t)) return -1; if (!kbStepAt) { kbStepAt = millis() | 1; return 0; } if (millis() - kbStepAt < 1000) return 0; kbStepAt = 0; if (!hw.kbThere) return 0; return kbConnect(a, t, 0, false) ? 1 : 0; }
static bool kbPoll(TwRep &r) {
  uint32_t ms = millis();
  for (int i = 0; i < hw.nKeys; i++) if (!hw.keys[i].done && ms >= hw.keys[i].at) {
    hw.keys[i].done = true; memset(&r, 0, sizeof r); r.n = 8; r.handle = 0x24; r.ms = ms;
    if (!hw.keys[i].up) { r.d[0] = hw.keys[i].mod; r.d[2] = hw.keys[i].usage; static int downs = 0; if (hw.cutAtKey && ++downs >= hw.cutAtKey) { downs = 0; _exit(0); } }   // the bench's power cut
    hw.kbReports++; return true;
  }
  return false;
}
#endif

// ---- the pairing page (turned like the typewriter)
static void drawKb() {
  textLeft(EK_BIG, 20, 12, L("Keyboard", "Clavier"));
  rect(12, 56, W - 24, 2, true);
  if (kbPass) {
    textAt(EK_MID, W / 2, 120, L("Type this code on the keyboard, then Enter:", "Tapez ce code sur le clavier, puis Entr\xC3\xA9" "e\xC2\xA0:"));
    char b[12]; snprintf(b, sizeof b, "%06lu", (unsigned long)kbPass); textAt(EK_BIG, W / 2, 180, b, 2);
    return;
  }
  if (kbMsg.length()) { para(EK_MID, 24, 80, W - 48, kbMsg, 10); }
  else if (kbFound.empty()) para(EK_MID, 24, 80, W - 48, String(L("No Bluetooth keyboard answered. ", "Aucun clavier Bluetooth n'a r\xC3\xA9pondu. ")) + kbOthers + L(" other devices heard. Put the keyboard in pairing mode, then a long press to listen again.", " autres appareils entendus. Mettez le clavier en appairage, puis un appui long pour \xC3\xA9" "couter \xC3\xA0 nouveau."), 10);
  for (int i = 0; i < (int)kbFound.size() && i < 7; i++) {
    int y = 72 + i * 48; const KbFound &f = kbFound[i];
    textLeft(EK_MID, 28, y + 10, String(i + 1) + "  " + (f.name[0] ? String(f.name) : String(L("(no name)", "(sans nom)"))), true, W - 200);
    textRight(EK_SMALL, W - 28, y + 14, String(f.rssi) + " dBm");
    if (i == kbCur) invert(16, y + 2, W - 32, 44);
  }
  textLeft(EK_SMALL, 16, H - 26, L("dial: choose  \xC2\xB7  press or PWR: pair  \xC2\xB7  long press: listen again  \xC2\xB7  long press twice: leave",
                                   "molette\xC2\xA0: choisir  \xC2\xB7  appui ou PWR\xC2\xA0: appairer  \xC2\xB7  appui long\xC2\xA0: \xC3\xA9" "couter  \xC2\xB7  deux fois\xC2\xA0: quitter"), true, W - 32);
}

// ---- the session
static void twSessionEnd(const char *why) {
  if (!twSessionAt) return;
  uint32_t mins = (millis() - twSessionAt) / 60000;
  FL("type", String("session ") + why + ": " + mins + " min, " + twSessionLines + " lines saved, " + twSessionKeys + " characters" +
             (twLatN ? ", key to paper median " + String(twMedian()) + " ms, last " + String(twLastLat) + " ms (" + String(twLatN) + " refreshes)" : String("")));
  twSessionAt = 0;
}
static void twLeave() {
  twSessionEnd("ended");
  sdEnd();                                                           // v0.4.0: the keyboard stays, for the tiles
  keySink = NULL;
  R.scr = SCR_TILE; R.tile = T_TYPE;
  setRot(0); havePrev = false; epdHot = false;
}
// show the keyboard list and pair; true when a keyboard is ready
static bool twPairFlow() {
  R.scr = SCR_KB; kbMsg = L("Listening for Bluetooth keyboards for 6 seconds. Put the keyboard in pairing mode now.", "\xC3\x89" "coute des claviers Bluetooth pendant 6 secondes. Mettez le clavier en appairage.");
  clearFb(); drawKb(); push(true);
  kbScan(); kbMsg = "";
  clearFb(); drawKb(); push(false);
  uint32_t idle = millis(); int longs = 0;
  while (millis() - idle < 120000) {
    bool pairNow = false;
    if (btn(PIN_UP) || btn(PIN_DOWN)) { int dir = btn(PIN_UP) ? -1 : 1; uint32_t t0 = millis(); while ((btn(PIN_UP) || btn(PIN_DOWN)) && millis() - t0 < 400) delay(8);
      if (!kbFound.empty()) { kbCur = (kbCur + dir + kbFound.size()) % kbFound.size(); clearFb(); drawKb(); push(false); } idle = millis(); }
    if (btn(PIN_PRESS)) { uint32_t t0 = millis(); while (btn(PIN_PRESS)) delay(8); idle = millis();
      if (millis() - t0 > 700) { if (++longs >= 2) return false; kbMsg = L("Listening again...", "\xC3\x89" "coute \xC3\xA0 nouveau..."); clearFb(); drawKb(); push(false); kbScan(); kbMsg = ""; clearFb(); drawKb(); push(false); continue; }
      pairNow = true; }
    if (pwrShortPressed()) pairNow = true;
    if (pairNow && !kbFound.empty()) {
      KbFound f = kbFound[kbCur];
      kbMsg = L("Connecting. If the keyboard asks for a code, it appears here.", "Connexion. Si le clavier demande un code, il s'affiche ici."); clearFb(); drawKb(); push(false);
      if (kbConnect(f.a, f.type, 15000, true)) {
        prefs.putBytes("kbA", f.a, 6); prefs.putUChar("kbT", f.type);
        FL("keyboard", "paired and bonded");                  // never its name or address
        return true;
      }
      kbMsg = L("The keyboard did not pair. Press to try again, long press to listen again.", "Le clavier ne s'est pas appair\xC3\xA9. Appui\xC2\xA0: r\xC3\xA9" "essayer, appui long\xC2\xA0: \xC3\xA9" "couter.");
      FL("keyboard", "pairing failed");
      clearFb(); drawKb(); push(false); kbMsg = "";
    }
    delay(10);
  }
  return false;
}
// into the typewriter: the page turns, the card mounts, the keyboard comes (paired before, or now)
static void twEnter(bool fromWake) {
  setRot(prefs.getUChar("twRot", 1) == 2 ? 2 : 1);
  if (!twQ) twQ = xQueueCreate(128, sizeof(TwRep));
  twCard = sdBegin();
  if (!fromWake) { FL("card", twCard ? "microSD mounted" : "no microSD"); }
  twLoadToday();
  R.scr = SCR_TYPE; havePrev = false;
  uint8_t a[6], t;
  if (!kbSaved(a, t)) {
    if (!twPairFlow()) { twLeave(); return; }
  } else if (!kbConnected()) {
    R.scr = SCR_KB; kbMsg = L("Press a key on the keyboard so it calls back. The dial: leave.", "Appuyez sur une touche du clavier pour qu'il se reconnecte. La molette\xC2\xA0: quitter.");
    clearFb(); drawKb(); push(true); kbMsg = "";
    uint32_t t0 = millis();
    if (!kbListen(90000)) { FL("keyboard", "did not call back within 90 s"); twLeave(); return; }
    FL("keyboard", String("back in ") + String((millis() - t0) / 1000.0f, 1) + " s, no new pairing");
  }
  keySink = NULL;                                                    // v0.3.0: the keys are the typewriter's
  R.scr = SCR_TYPE; twSessionAt = millis(); twSessionLines = twSessionKeys = 0; twLatN = 0; twLastLat = 0; twWindows = 0;
  twLastInput = millis(); memset(twPrevKeys, 0, 6);
  FL("type", "session started");
  clearFb(); drawType(); push(true); twWindows = 0;
}
static void twLoop() {
  sampleIfDue(); if (kbConnected()) R.kbOn = 1;                       // v0.3.0
  TwRep r; bool got = false;
  while (kbPoll(r)) { twReport(r); got = true; }             // every key that waited, at once
  if (twEscLeave) { twEscLeave = false; twLeave(); freshen(); render(); push(true); lastInput = millis(); shKbStart(); return; }   // v0.4.0
  if (kbBatNew >= 0) { R.kbBat = kbBatNew; kbBatNew = -1; }
  if (got || twKeyAt) {
    bool full = twCleanNext || (twWindows >= TW_FULL_EVERY && R.tline[0] == 0);   // Enter, or now and then between lines: a clean page
    twCleanNext = false;
    twPaper(full);
  }
  if (btn(PIN_PRESS)) {
    uint32_t t0 = millis(); while (btn(PIN_PRESS)) delay(8); twLastInput = millis();
    if (millis() - t0 >= LONG_MS) { twLeave(); freshen(); render(); push(true); lastInput = millis(); return; }
    if (twLatN) twShownLat = twMedian();
    clearFb(); drawType(); push(true); twWindows = 0;                   // a short press: a clean page
  }
  if (!kbConnected()) {
    twSessionEnd("paused (keyboard gone)");
    R.scr = SCR_KB; kbMsg = L("The keyboard went away. Press a key on it so it calls back. The dial: leave.", "Le clavier est parti. Appuyez sur une touche pour qu'il revienne. La molette\xC2\xA0: quitter.");
    clearFb(); drawKb(); push(false); kbMsg = "";
    if (kbListen(90000)) { R.scr = SCR_TYPE; twSessionAt = millis(); twLastInput = millis(); clearFb(); drawType(); push(true); twWindows = 0; FL("keyboard", "back, no new pairing"); }
    else { twLeave(); freshen(); render(); push(true); lastInput = millis(); }
    return;
  }
  if (millis() - twLastInput > TW_IDLE_MS) {
    twSessionEnd("ended after 10 min without a key");
    kbOff(); sdEnd();
    clearFb(); drawType(); push(true);                                  // the page stays on the paper while it sleeps
    epdHot = false; goSleep();
  }
}

// ================================================================ the archives (v0.3.0)
// Kiwix archives turned into plain text by shelf_convert.py on a computer, read from the microSD:
//   /shelf/<name>/about.txt    name, language, source, date, licence (key=value)
//   /shelf/<name>/titles.txt   key TAB title TAB file TAB offset TAB length TAB redirect target, sorted by key
//   /shelf/<name>/text-NNN.txt the articles: a title line, then the body ("## " headings, "• " items, blank lines between)
// The title search jumps into the middle of titles.txt and halves the distance (no database, any size up to 4 GB).
// With the keyboard: type, the list follows. Without: the dial picks a first letter, then a title.
// Every article shows its archive's source and licence on its first page.
#define SH_ROWS 13
#define SH_ROW_H 46
#define SH_LIST_Y 104
#define SH_LINE 1024                           // the longest line read at once from titles.txt
#define SH_ART_CAP 300000                      // bytes of one article read into memory; longer ones are cut, and say so
#define SH_FULL_EVERY 1                        // page turns between clean full refreshes (1: every turn is clean)
static const char *SH_LETTERS = "abcdefghijklmnopqrstuvwxyz0123456789";
// shDirs, shTitlesOf and shScanned (the archives on the card, their titles) are declared with the lists
static ShArcInfo shA; static String shALoaded;
static String shErr;
static bool shDirty = false, shFullNext = false;
static uint32_t shListenUntil = 0, shTurns = 0;
static bool shScreen() { return R.scr == SCR_FIND || R.scr == SCR_READ; }

// ---- the card: one file kept open, reads at any offset
#ifndef EK_HOST
static File shF; static String shFP;
static bool shOpenF(const String &p) { if (shF && shFP == p) return true; if (shF) shF.close(); shF = SD_MMC.open(p, "r"); shFP = p; return (bool)shF; }
static int shReadAt(const String &p, uint32_t off, uint8_t *b, int n) { if (!shOpenF(p)) return -1; if (!shF.seek(off)) return 0; return shF.read(b, n); }
static uint32_t shSize(const String &p) { return shOpenF(p) ? (uint32_t)shF.size() : 0; }
static void shClose() { if (shF) shF.close(); shFP = ""; }
static bool shExists(const String &p) { return SD_MMC.exists(p); }
static void shList(std::vector<String> &out) {
  out.clear(); File d = SD_MMC.open("/shelf"); if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) { if (f.isDirectory()) { String n = f.name(); int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s + 1); out.push_back(n); } f.close(); }
  d.close();
}
#else
static FILE *shF = NULL; static String shFP;
static bool shOpenF(const String &p) { if (shF && shFP == p) return true; if (shF) fclose(shF); shF = hw.sdThere ? fopen(hostSd(p).c_str(), "rb") : NULL; shFP = p; return shF != NULL; }
static int shReadAt(const String &p, uint32_t off, uint8_t *b, int n) { if (!shOpenF(p)) return -1; if (fseek(shF, off, SEEK_SET)) return 0; return (int)fread(b, 1, n, shF); }
static uint32_t shSize(const String &p) { if (!shOpenF(p)) return 0; fseek(shF, 0, SEEK_END); return (uint32_t)ftell(shF); }
static void shClose() { if (shF) fclose(shF); shF = NULL; shFP = ""; }
static bool shExists(const String &p) { FILE *f = hw.sdThere ? fopen(hostSd(p).c_str(), "rb") : NULL; if (f) fclose(f); return f != NULL; }
static void shList(std::vector<String> &out) {
  out.clear(); if (!hw.sdThere) return; DIR *d = opendir(hostSd("/shelf").c_str()); if (!d) return;
  for (struct dirent *e = readdir(d); e; e = readdir(d)) if (e->d_name[0] != '.' && e->d_type == DT_DIR) out.push_back(String(e->d_name));
  closedir(d);
}
#endif
static String shPath(const char *file) { return "/shelf/" + String(R.shArc) + "/" + file; }
static String shTextPath(int k) { char b[20]; snprintf(b, sizeof b, "text-%03d.txt", k); return shPath(b); }

// ---- the archives on the card, and one archive's about.txt
static bool shReadAbout(const String &dir, ShArcInfo &a) {
  a = ShArcInfo(); a.dir = dir; a.format = 0;
  String p = "/shelf/" + dir + "/about.txt"; uint8_t b[2048]; int n = shReadAt(p, 0, b, sizeof b - 1); shClose();
  if (n <= 0) return false;
  b[n] = 0;
  String s = (const char *)b;
  int at = 0;
  while (at < (int)s.length()) {
    int e = s.indexOf('\n', at); if (e < 0) e = s.length();
    String ln = s.substring(at, e); at = e + 1;
    int q = ln.indexOf('='); if (q < 0) continue;
    String k = ln.substring(0, q), v = ln.substring(q + 1); v.trim();
    if (k == "title") a.title = v; else if (k == "language") a.lang = v; else if (k == "source") a.source = v;
    else if (k == "licence") a.licence = v; else if (k == "date") a.date = v; else if (k == "format") a.format = v.toInt();
  }
  if (!a.title.length()) a.title = dir;
  return a.format == 1;
}
static void shScan() {
  shDirs.clear(); shTitlesOf.clear(); shScanned = true;
  if (!sdBegin()) { R.shN = 0; R.shCard = 0; return; }
  R.shCard = 1;
  std::vector<String> all; shList(all); std::sort(all.begin(), all.end(), [](const String &a, const String &b) { return strcmp(a.c_str(), b.c_str()) < 0; });
  for (auto &d : all) { ShArcInfo a; if (shReadAbout(d, a)) { shDirs.push_back(d); shTitlesOf.push_back(a.title); } }
  R.shN = shDirs.size() > 250 ? 250 : shDirs.size();
}
static bool shArcReady() {                     // the open archive's about.txt, read once per wake
  if (!R.shArc[0]) return false;
  if (shALoaded == R.shArc) return true;
  if (!sdBegin()) { shErr = L("No card in the slot.", "Pas de carte dans la fente."); return false; }
  if (!shReadAbout(R.shArc, shA)) { shErr = L("This archive is missing, or was made by another version of the converter.", "Cette archive manque, ou vient d'une autre version du convertisseur."); return false; }
  shALoaded = R.shArc; shErr = "";
  return true;
}

// ---- the search key of what is typed (the converter folds titles the same way): a-z 0-9, the rest a space
static void shFold(const char *in, char *out, size_t cap) {
  static const char *const HI[64] = { "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o"," ","o","u","u","u","u","y","th","ss",
                                     "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o"," ","o","u","u","u","u","y","th","y" };
  uint16_t cp[80]; int n = decodeUtf8(in, cp, 80); size_t k = 0; bool sp = true;
  auto put = [&](const char *s) { for (; *s && k + 1 < cap; s++) { if (*s == ' ') { if (!sp) { out[k++] = ' '; sp = true; } } else { out[k++] = *s; sp = false; } } };
  for (int i = 0; i < n; i++) {
    uint16_t c = cp[i]; char one[2] = { 0, 0 };
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { one[0] = (char)c; put(one); }
    else if (c >= 'A' && c <= 'Z') { one[0] = (char)(c + 32); put(one); }
    else if (c >= 0xC0 && c <= 0xFF) put(HI[c - 0xC0]);
    else if (c == 0xB2) put("2"); else if (c == 0xB3) put("3"); else if (c == 0xB9) put("1"); else if (c == 0xAA) put("a"); else if (c == 0xBA) put("o");
    else put(" ");
  }
  out[k] = 0;
}

// ---- titles.txt: one line at an offset, and the first line whose key is not below the query
static uint8_t shBuf[SH_LINE + 1];
static bool shLineAt(uint32_t pos, String &key, uint32_t &next, String *rest = NULL) {
  int n = shReadAt(shPath("titles.txt"), pos, shBuf, SH_LINE); if (n <= 0) return false;
  shBuf[n] = 0; int nl = -1, tab = -1;
  for (int i = 0; i < n; i++) { if (shBuf[i] == '\t' && tab < 0) tab = i; if (shBuf[i] == '\n') { nl = i; break; } }
  next = pos + (nl >= 0 ? nl + 1 : n);
  if (nl >= 0) shBuf[nl] = 0;
  if (tab >= 0) { shBuf[tab] = 0; key = (const char *)shBuf; if (rest) *rest = (const char *)shBuf + tab + 1; }
  else { key = (const char *)shBuf; if (rest) *rest = ""; }
  return true;
}
static uint32_t shNextStart(uint32_t pos) {   // the first line start at or after pos
  if (pos == 0) return 0;
  int n = shReadAt(shPath("titles.txt"), pos - 1, shBuf, SH_LINE); if (n <= 0) return pos;
  for (int i = 0; i < n; i++) if (shBuf[i] == '\n') return pos + i;
  return pos - 1 + n;
}
static uint32_t shPrevStart(uint32_t pos) {   // the line start before the line at pos
  if (pos == 0) return 0;
  uint32_t from = pos > SH_LINE ? pos - SH_LINE : 0; int n = shReadAt(shPath("titles.txt"), from, shBuf, pos - from); if (n <= 0) return 0;
  for (int i = n - 2; i >= 0; i--) if (shBuf[i] == '\n') return from + i + 1;
  return from;
}
static uint32_t shLower(const char *q) {
  uint32_t size = shSize(shPath("titles.txt")), lo = 0, hi = size; String k; uint32_t nx;
  if (!q[0]) return 0;
  for (int it = 0; it < 64 && hi - lo > 2 * SH_LINE; it++) {
    uint32_t mid = lo + (hi - lo) / 2, ls = shNextStart(mid);
    if (ls >= hi) break;
    if (!shLineAt(ls, k, nx)) break;
    if (strcmp(k.c_str(), q) < 0) lo = nx; else hi = ls;
  }
  for (uint32_t pos = lo; pos < hi;) { if (!shLineAt(pos, k, nx)) break; if (strcmp(k.c_str(), q) >= 0) return pos; if (nx <= pos) break; pos = nx; }
  return hi;
}
static bool shParse(uint32_t pos, ShLine &L) {
  String rest; if (!shLineAt(pos, L.key, L.next, &rest)) return false;
  L.at = pos;
  int t1 = rest.indexOf('\t'), t2 = rest.indexOf('\t', t1 + 1), t3 = rest.indexOf('\t', t2 + 1), t4 = rest.indexOf('\t', t3 + 1);
  if (t1 < 0 || t2 < 0 || t3 < 0) return false;
  L.title = rest.substring(0, t1); L.file = rest.substring(t1 + 1, t2).toInt(); L.off = strtoul(rest.substring(t2 + 1, t3).c_str(), NULL, 10);
  L.len = strtoul(rest.substring(t3 + 1, t4 < 0 ? rest.length() : t4).c_str(), NULL, 10);
  L.target = t4 < 0 ? String("") : rest.substring(t4 + 1);
  return true;
}
static bool shMatches(const ShLine &L) { size_t n = strlen(R.shKey); return !n || !strncmp(L.key.c_str(), R.shKey, n); }

// ---- the rows shown: from R.shAt, while the key starts with the query
static ShLine shRows[SH_ROWS]; static int shNRows = 0; static bool shMore = false;
static String shRowsFor; static uint32_t shRowsAt = 0xFFFFFFFF;
static void shFill() {
  String tag = String(R.shArc) + "|" + R.shKey;
  if (shRowsFor == tag && shRowsAt == R.shAt) return;
  shRowsFor = tag; shRowsAt = R.shAt; shNRows = 0; shMore = false;
  if (!shArcReady()) return;
  uint32_t size = shSize(shPath("titles.txt")), pos = R.shAt;
  while (pos < size && shNRows <= SH_ROWS) {
    ShLine L; if (!shParse(pos, L) || !shMatches(L)) break;
    if (shNRows == SH_ROWS) { shMore = true; break; }
    shRows[shNRows++] = L; if (L.next <= pos) break; pos = L.next;
  }
  if (R.shCur >= shNRows) R.shCur = shNRows ? shNRows - 1 : 0;
}
static void shQuery() {                        // the query changed: the list starts again at its first match
  shFold(R.shQ, R.shKey, sizeof R.shKey);
  R.shLb = shArcReady() ? shLower(R.shKey) : 0; R.shAt = R.shLb; R.shCur = 0; shRowsAt = 0xFFFFFFFF;
}
static void shMove(int d) {
  shFill();
  if (d > 0) { if (R.shCur + 1 < shNRows) R.shCur++; else if (shMore && shNRows > 1) { R.shAt = shRows[1].at; } }
  else { if (R.shCur > 0) R.shCur--; else if (R.shAt > R.shLb) R.shAt = shPrevStart(R.shAt); }
}

// ---- one article: read, decoded, cut into pages
static char *rdRaw = NULL; static uint16_t *rdT = NULL; static int rdN = 0, rdBody = 0; static bool rdCut = false;
static std::vector<uint32_t> rdPages; static String rdFor;
static String rdTitle;
#define RD_GAP (EK_MID.height / 2 + 2)
#define RD_BOTTOM (H - 52)
static int rdTitleLines() { static int st[4], en[4]; int n = decodeUtf8(rdTitle.c_str(), tbuf, 400); return max(1, wrap(EK_BIG, tbuf, n, W - 40, st, en, 3)); }
static String shCredit();
static int rdCreditLines() { static int st[3], en[3]; int n = decodeUtf8(shCredit().c_str(), tbuf, 400); return max(1, wrap(EK_SMALL, tbuf, n, W - 40, st, en, 2)); }
static int rdHead() { return 38 + rdTitleLines() * EK_BIG.height + 4 + rdCreditLines() * EK_SMALL.height + 8; }   // the first page's title block
static int rdTop(int page) { return page == 0 ? rdHead() + 14 : 60; }
// lays out (and draws, if asked) the body from p; returns where the next page starts
static int rdFill(int p, int y0, int yMax, bool draw) {
  static int st[80], en[80];
  int y = y0;
  while (p < rdN) {
    int ps = p; while (ps > rdBody && rdT[ps - 1] != '\n') ps--;
    int e = p; while (e < rdN && rdT[e] != '\n') e++;
    if (e == ps) { if (y > y0) y += RD_GAP; p = e + 1; continue; }        // a blank line: a little space, none at the top of a page
    bool head = e - ps > 3 && rdT[ps] == '#' && rdT[ps + 1] == '#' && rdT[ps + 2] == ' ';
    bool item = e - ps > 2 && rdT[ps] == 0x2022 && rdT[ps + 1] == ' ';
    int ts = p == ps ? ps + (head ? 3 : item ? 2 : 0) : p;
    const EkFont &f = head ? EK_BIG : EK_MID; int lh = f.height + (head ? 6 : 4);
    int x = CARD_X + (item ? 28 : 0), w = CARD_W - (item ? 28 : 0);
    if (head && p == ps && y > y0) y += RD_GAP;
    int room = (yMax - y) / lh; if (room > 60) room = 60;
    if ((room <= 0 || (head && room < 2)) && y > y0) return p;           // no room: the next page starts here
    if (room <= 0) room = 1;
    int k = wrap(f, rdT + ts, e - ts, w, st, en, room + 1);
    int here = k < room ? k : room;
    if (draw) for (int l = 0; l < here; l++) {
      int xx = x; if (item && l == 0 && ts == ps + 2) glyph(EK_MID, CARD_X + 6, y, 0x2022, true);
      for (int c = st[l]; c < en[l]; c++) { glyph(f, xx, y, rdT[ts + c], true); xx += glyphW(f, rdT[ts + c]); }
      if (head) rect(x, y + f.ascent + 5, xx - x, 2, true);
      y += lh;
    } else y += here * lh;
    if (k > room) return ts + st[room];
    if (k == 0 && e == ts) { p = e + 1; continue; }
    p = e + 1;
  }
  return rdN;
}
static bool rdLoad() {
  char tag[80]; snprintf(tag, sizeof tag, "%s|%u|%lu", R.shArc, (unsigned)R.shFile, (unsigned long)R.shOff);
  if (rdFor == tag && rdT) return true;
  rdFor = ""; rdN = 0; rdPages.clear();
  if (!shArcReady()) return false;
  if (!rdRaw) rdRaw = (char *)heap_caps_malloc(SH_ART_CAP + 64, MALLOC_CAP_SPIRAM);
  if (!rdT) rdT = (uint16_t *)heap_caps_malloc((SH_ART_CAP + 64) * 2, MALLOC_CAP_SPIRAM);
  if (!rdRaw || !rdT) { shErr = L("Not enough memory for this article.", "Pas assez de m\xC3\xA9moire pour cet article."); return false; }
  uint32_t want = R.shLen > SH_ART_CAP ? SH_ART_CAP : R.shLen; rdCut = R.shLen > SH_ART_CAP;
  int n = shReadAt(shTextPath(R.shFile), R.shOff, (uint8_t *)rdRaw, want);
  if (n <= 0) { shErr = L("The article could not be read from the card.", "L'article n'a pas pu \xC3\xAAtre lu sur la carte."); return false; }
  while (rdCut && n > 0 && ((uint8_t)rdRaw[n - 1] & 0xC0) == 0x80) n--;              // never cut a letter in half
  if (rdCut && n > 0 && (uint8_t)rdRaw[n - 1] >= 0xC0) n--;
  rdRaw[n] = 0;
  rdN = decodeUtf8(rdRaw, rdT, SH_ART_CAP + 60);
  int nl = 0; while (nl < rdN && rdT[nl] != '\n') nl++;
  rdTitle = ""; { char *q = strchr(rdRaw, '\n'); if (q) { *q = 0; rdTitle = rdRaw; *q = '\n'; } else rdTitle = rdRaw; }
  rdBody = nl < rdN ? nl + 1 : rdN;
  if (rdCut) { const char *m = L("\n\n(The rest of this article is too long for the 397: it stops here.)", "\n\n(La suite de cet article est trop longue pour la 397\xC2\xA0: il s'arr\xC3\xAAte ici.)");
    rdN += decodeUtf8(m, rdT + rdN, 200); }
  // the pages
  int p = rdBody; rdPages.push_back(p);
  while (p < rdN && rdPages.size() < 4000) {
    int q = rdFill(p, rdTop(rdPages.size() - 1), RD_BOTTOM, false);
    if (q <= p) q = p + 1;
    p = q; while (p < rdN && (rdT[p] == '\n' || rdT[p] == ' ')) p++;
    if (p < rdN) rdPages.push_back(p);
  }
  rdFor = tag;
  if (R.shPage >= (int)rdPages.size()) R.shPage = rdPages.size() - 1;
  if (R.shPage < 0) R.shPage = 0;
  return true;
}

// ---- the screens
static String shCredit() {
  String s = shA.source.length() ? shA.source : shA.title;
  if (shA.date.length()) s += ", " + shA.date;
  if (shA.licence.length()) s += "  \xC2\xB7  " + shA.licence;
  return s;
}
static void drawFind() {
  bool kb = kbConnected();
  if (!shArcReady()) { textLeft(EK_BIG, 20, 20, tileName(T_SHELF)); rect(16, 70, W - 32, 3, true); para(EK_MID, CARD_X, 110, CARD_W, shErr, 8); textAt(EK_SMALL, W / 2, H - 28, L("long press: tiles", "appui long\xC2\xA0: tuiles")); return; }
  shFill();
  textLeft(EK_SMALL, 16, 10, shA.title, true, W - 170);
  textRight(EK_SMALL, W - 16, 10, kb ? (R.kbBat >= 0 ? String(L("keyboard ", "clavier ")) + (int)R.kbBat + " %" : String(L("keyboard", "clavier")))
                                     : (shListenUntil && (int32_t)(shListenUntil - millis()) > 0 ? String(L("listening for the keyboard", "\xC3\xA9" "coute du clavier")) : String(L("dial", "molette"))));
  if (kb || R.shFocus) {
    String q = R.shQ;
    if (kb) {
      String cur = q + (R.dead ? deadChar(R.dead) : "");
      int w = textLeft(EK_BIG, 20, 42, cur, true, W - 60);
      rect(22 + w, 44, 4, EK_BIG.height - 4, true);
      if (!q.length()) textLeft(EK_MID, 44 + w, 50, L("type a title", "tapez un titre"));
      if (R.caps) textRight(EK_SMALL, W - 16, 52, "CAPS");
    } else if (strlen(R.shQ) > 1) textLeft(EK_BIG, 20, 42, q, true, W - 40);              // typed before: kept, the dial moves in it
    else textLeft(EK_BIG, 20, 42, String(L("Letter ", "Lettre ")) + String(R.shQ).substring(0, 1) + "  \xE2\x80\xA6");
  } else {
    char b[8]; snprintf(b, sizeof b, "%c", (char)toupper(R.shQ[0] ? R.shQ[0] : 'a'));
    textAt(EK_BIG, W / 2, 42, String("\xE2\x80\xB9   ") + b + "   \xE2\x80\xBA");
  }
  rect(16, 92, W - 32, 3, true);
  if (!shNRows) para(EK_MID, CARD_X, SH_LIST_Y + 20, CARD_W, String(L("No title starts with \xE2\x80\x9C", "Aucun titre ne commence par \xC2\xAB\xC2\xA0")) + R.shQ + L("\xE2\x80\x9D.", "\xC2\xA0\xC2\xBB."), 4);
  for (int r = 0; r < shNRows; r++) {
    int y = SH_LIST_Y + r * SH_ROW_H; const ShLine &L = shRows[r];
    String s = L.title; if (L.target.length()) s += "  \xE2\x86\x92 " + L.target;
    textLeft(EK_MID, 24, y + 10, s, true, W - 48);
    if (r == R.shCur && (kb || R.shFocus)) invert(16, y + 2, W - 32, SH_ROW_H - 4);
  }
  if (R.shAt > R.shLb) for (int k = 0; k < 8; k++) rect(W / 2 - k, 96 + k, 2 * k + 1, 1, true);
  if (shMore) for (int k = 0; k < 8; k++) rect(W / 2 - k, H - 42 - k, 2 * k + 1, 1, true);
  String foot = kb ? L("type to search  \xC2\xB7  \xE2\x86\x91\xE2\x86\x93  \xC2\xB7  Enter: read  \xC2\xB7  Tab: next archive  \xC2\xB7  Esc: tiles",
                       "tapez pour chercher  \xC2\xB7  \xE2\x86\x91\xE2\x86\x93  \xC2\xB7  Entr\xC3\xA9" "e\xC2\xA0: lire  \xC2\xB7  Tab\xC2\xA0: archive suivante  \xC2\xB7  \xC3\x89" "chap\xC2\xA0: tuiles")
             : R.shFocus ? L("dial: move  \xC2\xB7  press: read  \xC2\xB7  long press: letter", "molette\xC2\xA0: choisir  \xC2\xB7  appui\xC2\xA0: lire  \xC2\xB7  long\xC2\xA0: lettre")
                         : L("dial: letter  \xC2\xB7  press: the titles  \xC2\xB7  long press: tiles", "molette\xC2\xA0: lettre  \xC2\xB7  appui\xC2\xA0: les titres  \xC2\xB7  long\xC2\xA0: tuiles");
  textLeft(EK_SMALL, 16, H - 28, foot, true, W - 32);
}
static void drawRead() {
  if (!rdLoad()) { textLeft(EK_BIG, 20, 20, tileName(T_SHELF)); rect(16, 70, W - 32, 3, true); para(EK_MID, CARD_X, 110, CARD_W, shErr, 8); return; }
  int pg = R.shPage, np = rdPages.size();
  if (pg == 0) {
    textLeft(EK_SMALL, 16, 10, shA.title, true, W - 32);
    int th = para(EK_BIG, 20, 38, W - 40, rdTitle, 3);
    para(EK_SMALL, 20, 42 + th, W - 40, shCredit(), 2);
    rect(16, rdHead(), W - 32, 3, true);
  } else {
    textLeft(EK_SMALL, 16, 14, rdTitle, true, W - 120);
    textRight(EK_SMALL, W - 16, 14, String(pg + 1) + "/" + np);
    rect(16, 42, W - 32, 2, true);
  }
  rdFill(rdPages[pg], rdTop(pg), RD_BOTTOM, true);
  String foot = String(L("page ", "page ")) + (pg + 1) + "/" + np + "  \xC2\xB7  " +
                (kbConnected() ? L("space: next  \xC2\xB7  Esc: titles", "espace\xC2\xA0: suite  \xC2\xB7  \xC3\x89" "chap\xC2\xA0: titres") : L("dial: turn  \xC2\xB7  press: titles", "molette\xC2\xA0: tourner  \xC2\xB7  appui\xC2\xA0: titres"));
  textAt(EK_SMALL, W / 2, H - 28, foot);
}
static void faceShelf() {
  topBar();
  textAt(EK_BIG, W / 2, 50, tileName(T_SHELF));
  bookMark(W / 2, 200, 2); bookMark(W / 2, 260, 2);
  int y = 330;
  String s;
  if (R.shCard == 255) s = L("Kiwix archives turned into text, read from the microSD. Press to look at the card.", "Des archives Kiwix en texte, lues sur la microSD. Appui\xC2\xA0: regarder la carte.");
  else if (!R.shCard) s = L("No card in the slot.", "Pas de carte dans la fente.");
  else if (!R.shN) s = L("No archive on the card yet. On a computer, shelf_convert.py writes them into /shelf.", "Pas encore d'archive sur la carte. Sur un ordinateur, shelf_convert.py les \xC3\xA9" "crit dans /shelf.");
  else s = String(R.shN) + L(R.shN == 1 ? " archive on the card" : " archives on the card", R.shN == 1 ? " archive sur la carte" : " archives sur la carte");
  y += para(EK_MID, 40, y, W - 80, s, 6, true) + 20;
  if (R.shArc[0] && R.shN) textAt(EK_MID, W / 2, y, String(L("last open: ", "derni\xC3\xA8re\xC2\xA0: ")) + R.shAT);
  y += 40;
  textAt(EK_MID, W / 2, y, kbSavedQuick() ? L("with the keyboard: type a title", "avec le clavier\xC2\xA0: tapez un titre") : L("no keyboard: the dial picks a letter", "sans clavier\xC2\xA0: la molette choisit une lettre"));
  textAt(EK_SMALL, W / 2, H - 30, L("press: open", "appui\xC2\xA0: ouvrir"));
}

// ---- moving between them
static void shKbStart() {                      // the keyboard, if one is paired: listened for in short rounds
  if (!twQ) twQ = xQueueCreate(128, sizeof(TwRep));
  if (kbSavedQuick() && !kbConnected()) { if (!shListenUntil) memset(twPrevKeys, 0, 6); shListenUntil = millis() + 60000; } else shListenUntil = 0;
}
static void shOpenArc(int i) {
  if (i < 0 || i >= (int)shDirs.size()) return;
  bool same = !strcmp(R.shArc, shDirs[i].c_str());
  strncpy(R.shArc, shDirs[i].c_str(), sizeof R.shArc - 1); R.shArc[sizeof R.shArc - 1] = 0;
  strncpy(R.shAT, shTitlesOf[i].c_str(), sizeof R.shAT - 1); R.shAT[sizeof R.shAT - 1] = 0;
  shALoaded = ""; shRowsAt = 0xFFFFFFFF;
  if (!same || !R.shQ[0]) { strcpy(R.shQ, kbConnected() ? "" : "a"); }
  R.scr = SCR_FIND; R.shFocus = 0;
  shQuery();
  FL("shelf", String("archive opened (") + R.shN + " on the card)");          // which one stays private: the folder name is the person's
  shKbStart(); shFullNext = true;
}
static void shEnter() {
  shScan();
  if (!R.shCard) { FL("shelf", "no microSD"); return; }
  if (!R.shN) { FL("shelf", "no archive on the card"); return; }
  if (R.shN == 1) { shOpenArc(0); return; }
  openList(L_ARCH);
  for (int i = 0; i < (int)shDirs.size(); i++) if (!strcmp(shDirs[i].c_str(), R.shArc)) R.cur = i;
}
static void shLeave() {
  shClose(); sdEnd(); keySink = NULL;                                // v0.4.0: the keyboard stays, for the tiles
  R.scr = SCR_TILE; R.tile = T_SHELF; shFullNext = true;
}
static void shOpenRow() {
  shFill(); if (!shNRows) return;
  const ShLine &L = shRows[R.shCur];
  R.shFile = L.file; R.shOff = L.off; R.shLen = L.len; R.shPage = 0; R.scr = SCR_READ; shFullNext = true; shTurns = 0;
  FL("shelf", "article opened");                                         // never which one
}
static void shTurn(int d) {
  if (!rdLoad()) return;
  int np = rdPages.size(), was = R.shPage;
  R.shPage = constrain(R.shPage + d, 0, np - 1);
  if (R.shPage != was && ++shTurns % SH_FULL_EVERY == 0) shFullNext = true;
}
static void shBack() { R.scr = SCR_FIND; shFullNext = true; }
// the dial on the archive screens
static void shAct(int ev) {
  if (R.scr == SCR_READ) {
    if (ev == EV_DOWN) shTurn(1); else if (ev == EV_UP) shTurn(-1); else shBack();
    return;
  }
  if (!shArcReady()) { if (ev == EV_LONG || ev == EV_PRESS) shLeave(); return; }
  bool kb = kbConnected();
  if (!kb && !R.shFocus) {                     // the letter wheel
    const char *p = strchr(SH_LETTERS, R.shQ[0] ? R.shQ[0] : 'a'); int i = p ? p - SH_LETTERS : 0, n = strlen(SH_LETTERS);
    if (ev == EV_UP) i = (i + n - 1) % n; else if (ev == EV_DOWN) i = (i + 1) % n;
    else if (ev == EV_PRESS) { shFill(); if (shNRows) { R.shFocus = 1; R.shCur = 0; } return; }
    else { shLeave(); return; }
    R.shQ[0] = SH_LETTERS[i]; R.shQ[1] = 0; shQuery();
    return;
  }
  if (ev == EV_DOWN) shMove(1); else if (ev == EV_UP) shMove(-1);
  else if (ev == EV_PRESS) shOpenRow();
  else if (!kb) { R.shFocus = 0; if (!R.shQ[0] || R.shQ[1]) { R.shQ[1] = 0; if (!R.shQ[0]) strcpy(R.shQ, "a"); shQuery(); } }
  else shLeave();
}
// the keyboard on the archive screens
static void shKey(uint8_t mod, uint8_t u) {
  lastInput = millis(); shDirty = true;
  if (R.scr == SCR_READ) {
    if (u == 0x2C || u == 0x4E || u == 0x51 || u == 0x4F || u == 0x28) shTurn(1);             // space, Page Down, down, right, Enter
    else if (u == 0x4B || u == 0x52 || u == 0x50) shTurn(-1);                                  // Page Up, up, left
    else if (u == 0x4A) { R.shPage = 0; shFullNext = true; } else if (u == 0x4D && rdLoad()) { R.shPage = rdPages.size() - 1; shFullNext = true; }
    else if (u == 0x29 || u == 0x2A) shBack();                                                 // Esc, Backspace
    return;
  }
  if (!shArcReady()) { if (u == 0x29) shLeave(); return; }
  bool altgr = (mod & 0x40) || ((mod & 0x11) && (mod & 0x04)), ctrl = (mod & 0x11) && !altgr;
  R.shFocus = 1;
  if (u == 0x29) { shLeave(); return; }
  if (u == 0x28 || u == 0x58) { shOpenRow(); return; }
  if (u == 0x51) { shMove(1); return; } if (u == 0x52) { shMove(-1); return; }
  if (u == 0x4E) { for (int i = 0; i < SH_ROWS - 1; i++) shMove(1); return; } if (u == 0x4B) { for (int i = 0; i < SH_ROWS - 1; i++) shMove(-1); return; }
  if (u == 0x2B) { if (shDirs.empty()) shScan(); int i = 0; for (int k = 0; k < (int)shDirs.size(); k++) if (!strcmp(shDirs[k].c_str(), R.shArc)) i = k;
    if (shDirs.size() > 1) { String q = R.shQ; shOpenArc((i + 1) % shDirs.size()); strncpy(R.shQ, q.c_str(), sizeof R.shQ - 1); shQuery(); } return; }   // Tab: the next archive, same query
  if (u == 0x39) { R.caps = !R.caps; return; }
  if (u == 0x2A) { if (R.dead) R.dead = 0; else { int n = strlen(R.shQ); if (n) { int c = n - 1; while (c > 0 && ((uint8_t)R.shQ[c] & 0xC0) == 0x80) c--; R.shQ[c] = 0; shQuery(); } } return; }
  if (ctrl) return;
  String t = deText(mod, u);
  if (!t.length()) return;
  size_t n = strlen(R.shQ);
  if (n + t.length() + 1 < sizeof R.shQ) { memcpy(R.shQ + n, t.c_str(), t.length() + 1); shQuery(); }
}
// one refresh: clean when asked, else partial; on the search page a pause in typing brings a clean page (partials grey here)
static uint16_t shPartials = 0; static uint32_t shLastDraw = 0;
static void shPush() {
  bool f = shFullNext; push(f); shFullNext = false;
  if (shScreen()) { shPartials = f ? 0 : shPartials + 1; shLastDraw = millis(); }
}
static void shSettle() { if (R.scr == SCR_FIND && shPartials && millis() - shLastDraw > 2000) { render(); push(true); shPartials = 0; } }
// awake on the archive screens: keys, the keyboard coming back, the samples
static void shKbBack() {                       // the keyboard is here: the letter of the wheel gives way to typing
  shListenUntil = 0; R.shFocus = 1; memset(twPrevKeys, 0, 6); shDirty = true; lastInput = millis();
  if (R.scr == SCR_FIND && strlen(R.shQ) <= 1) { R.shQ[0] = 0; shQuery(); }
  FL("keyboard", "back, no new pairing");
}
static void shPoll() {
  if (shListenUntil) {
    if (kbConnected()) shKbBack();
    else if ((int32_t)(shListenUntil - millis()) <= 0) { shListenUntil = 0; shDirty = true; }
    else if (kbListenStep() == 1) shKbBack();
  }
  keySink = shScreen() ? shKey : navKey;                              // v0.4.0
  TwRep r; while (kbPoll(r)) twReport(r);
  keySink = NULL; twKeyAt = 0;
  if (kbBatNew >= 0) { R.kbBat = kbBatNew; kbBatNew = -1; }
  if (kbConnected()) R.kbOn = 1;
}

// ================================================================ the keyboard everywhere (v0.4.0)
// The paired keyboard drives every screen, next to the dial. It cannot wake the 397 (the radio sleeps with it): after a
// person's wake (dial or PWR) the 397 listens for it a minute, in the background, and a key brings it back.
// Esc always does what a long press does. Tiles: ← → or 1-7, Enter opens. Lists: ↑ ↓, Page Up/Down, Home/End, Enter,
// a letter jumps to the next row that starts with it. Cards: space or → next, ← back, Enter shows the answer.
static void act(int ev);
static bool navDirty = false;                 // navEsc (Esc on the set-up page) is declared with the set-up page
static int navEvent(uint8_t u) {                // the keys every screen reads like the dial
  switch (u) {
    case 0x52: case 0x50: case 0x4B: return EV_UP;                    // up, left, Page Up
    case 0x51: case 0x4F: case 0x4E: case 0x2C: return EV_DOWN;       // down, right, Page Down, space
    case 0x28: case 0x58: return EV_PRESS;                            // Enter
    case 0x29: return EV_LONG;                                        // Esc
  }
  return EV_NONE;
}
static void navJump(char c) {                   // a letter in a list: the next row that starts with it
  int n = listCount(); if (!n) return;
  for (int k = 1; k <= n; k++) {
    int i = (R.cur + k) % n; String right; String s = listItem(i, right); char key[8]; shFold(s.c_str(), key, sizeof key);
    if (key[0] == c) { R.cur = i; return; }
  }
}
static void navKey(uint8_t mod, uint8_t u) {
  (void)mod;
  lastInput = millis(); navDirty = true;
  int ev = navEvent(u);
  switch (R.scr) {
    case SCR_TILE:
      if (u >= 0x1E && u <= 0x24 && u - 0x1E < N_TILES) { R.tile = u - 0x1E; return; }        // 1-7: straight to a tile
      if (u == 0x2C) ev = EV_PRESS;                                    // space opens too
      break;
    case SCR_LIST: {
      int n = listCount();
      if (u == 0x4B || u == 0x4E) { R.cur = constrain(R.cur + (u == 0x4E ? LIST_ROWS : -LIST_ROWS), 0, n ? n - 1 : 0); return; }   // a page
      if (u == 0x4A) { R.cur = 0; return; }                            // Home
      if (u == 0x4D) { R.cur = n ? n - 1 : 0; return; }               // End
      if (u == 0x50 || u == 0x2A) ev = EV_LONG;                        // left, Backspace: back
      if (u == 0x4F) ev = EV_PRESS;                                    // right: open
      if (u == 0x2C) ev = EV_PRESS;
      if (ev == EV_NONE) {                                             // a letter or digit: jump to the next row with it
        char c = 0;
        if (u >= 0x04 && u <= 0x1D) c = u == 0x1C ? 'z' : u == 0x1D ? 'y' : 'a' + (u - 0x04);
        else if (u >= 0x1E && u <= 0x27) c = u == 0x27 ? '0' : '1' + (u - 0x1E);
        if (c) navJump(c);
        return;
      }
      break;
    }
    case SCR_CARD:
      if (u == 0x28 || u == 0x58) {                                    // Enter: the answer, else the next page or card
        if (loadPack(R.slug) && R.card < nIn && !strcmp(jgs(cardN[inSec[R.card]], "type"), "prompt") && !R.revealed) ev = EV_PRESS; else ev = EV_DOWN;
      }
      break;
    case SCR_QR: if (ev == EV_LONG) ev = EV_PRESS; break;             // Esc or Enter: back to the list
    case SCR_DOC: case SCR_BAT: if (ev == EV_LONG || ev == EV_PRESS) ev = EV_PRESS; else ev = EV_NONE; break;
    default: break;
  }
  if (ev != EV_NONE) act(ev);
}
static void escSink(uint8_t, uint8_t u) { if (u == 0x29) navEsc = true; }

static void act(int ev) {
  if (ev == EV_NONE) return;
  ensurePack();
  switch (R.scr) {
    case SCR_TILE:
      if (ev == EV_UP) R.tile = (R.tile + N_TILES - 1) % N_TILES;
      else if (ev == EV_DOWN) R.tile = (R.tile + 1) % N_TILES;
      else if (ev == EV_LONG) R.tile = T_CLOCK;
      else if (R.tile == T_EDITIONS) openList(L_SHELF);
      else if (R.tile == T_CATALOG) openList(L_CATALOG);
      else if (R.tile == T_DEVICE) openList(L_DEVICE);
      else if (R.tile == T_TYPE) twEnter(false);                 // v0.2.0
      else if (R.tile == T_SHELF) shEnter();                      // v0.3.0
      else if (R.tile == T_CLIMATE) { readRoomNow(); goOnline("asked on the Climate tile"); }   // press: now, not in an hour
      break;
    case SCR_LIST: {
      int n = listCount();
      if (ev == EV_UP) R.cur = n ? (R.cur + n - 1) % n : 0;
      else if (ev == EV_DOWN) R.cur = n ? (R.cur + 1) % n : 0;
      else if (ev == EV_LONG) {
        if (R.list == L_ARCH) { R.scr = SCR_TILE; R.tile = T_SHELF; }
        else if (R.list == L_CARDS) { R.list = L_SECS; R.cur = R.sec < 0 ? nSecs : R.sec; R.top = 0; }
        else if (R.list == L_SECS) { R.list = L_SHELF; R.cur = 0; for (int i = 0; i < shelfN; i++) if (!strcmp(BUILTIN[shelf[i]].slug, R.slug)) R.cur = i; R.top = 0; }
        else R.scr = SCR_TILE;
      } else if (n) {
        if (R.list == L_SHELF) { strncpy(R.slug, BUILTIN[shelf[R.cur]].slug, sizeof R.slug - 1); if (loadPack(R.slug)) { FL("tile", String("pack opened: ") + R.slug); openList(L_SECS); } }
        else if (R.list == L_SECS) { R.sec = R.cur < nSecs ? R.cur : -1; pickSection(R.sec); R.list = L_CARDS; R.cur = 0; R.top = 0; }
        else if (R.list == L_CARDS) { R.scr = SCR_CARD; R.card = R.cur; R.page = 0; R.revealed = 0; }
        else if (R.list == L_CATALOG) { R.scr = SCR_QR; R.qr = R.cur; }
        else if (R.list == L_ARCH) shOpenArc(R.cur);                 // v0.3.0
        else {
          if (R.cur == 0) { render(); setupPage(); }
          else if (R.cur == 1) { R.fr = !R.fr; uiFr = R.fr; prefs.putString(EK_SET_LANG, R.fr ? "fr" : "en"); FL("setup", String("interface language set to ") + (R.fr ? "fr" : "en")); }
          else if (R.cur == 2) { prefs.putUChar("twRot", prefs.getUChar("twRot", 1) == 2 ? 1 : 2); }   // v0.2.0
          else if (R.cur == 3) { if (kbSavedQuick()) { kbOff(); kbForget(); } else { if (!twQ) twQ = xQueueCreate(128, sizeof(TwRep)); twPairFlow(); R.scr = SCR_LIST; R.list = L_DEVICE; R.cur = 3; keySink = NULL; } }   // v0.4.0: pair from here
          else if (R.cur == 4) { R.scr = SCR_DOC; }
          else if (R.cur == 5) { R.scr = SCR_BAT; }                  // v0.3.0
          else if (R.cur == 6) switchOff("Device menu");
          else R.scr = SCR_TILE;
        }
      }
      break;
    }
    case SCR_CARD: {
      if (!loadPack(R.slug)) { R.scr = SCR_TILE; break; }
      layoutCard();
      if (ev == EV_DOWN) { if (R.page + 1 < nPages) R.page++; else if (R.card + 1 < nIn) { R.card++; R.page = 0; R.revealed = 0; } }
      else if (ev == EV_UP) { if (R.page > 0) R.page--; else if (R.card > 0) { R.card--; R.revealed = 0; layoutCard(); R.page = nPages - 1; } }
      else if (ev == EV_PRESS && !strcmp(jgs(cardN[inSec[R.card]], "type"), "prompt") && !R.revealed) R.revealed = 1;
      else { R.scr = SCR_LIST; R.list = L_CARDS; R.cur = R.card; }
      break;
    }
    case SCR_QR: {
      int n = catCount();
      if (ev == EV_UP) R.qr = (R.qr + n - 1) % n; else if (ev == EV_DOWN) R.qr = (R.qr + 1) % n;
      else { R.scr = SCR_LIST; R.list = L_CATALOG; R.cur = R.qr; }
      break;
    }
    case SCR_DOC: R.scr = SCR_LIST; R.list = L_DEVICE; R.cur = 4; break;
    case SCR_BAT: R.scr = SCR_LIST; R.list = L_DEVICE; R.cur = 5; break;   // v0.3.0
    case SCR_FIND: case SCR_READ: shAct(ev); break;
    default: R.scr = SCR_TILE; break;
  }
}

// ================================================================ start, wake, sleep
static uint64_t sleepUs() {
  time_t now = time(NULL);
  if (R.scr == SCR_TILE && R.tile == T_CLOCK && timeValid()) return (uint64_t)(60 - now % 60 + 1) * 1000000ULL;   // the next minute
  uint32_t to30 = 1800 - (uint32_t)(now % 1800) + 2;
  return (uint64_t)to30 * 1000000ULL;
}
static void goSleep() {
  flFlush();
  uint64_t us = sleepUs();
  sleepFor(us);
}
static void housekeeping(bool personWake, bool mayFetch) {
  readBattery();
  // battery empty: say so on the paper and switch off, so a dead unit never looks alive
  if (R.batPct >= 0 && R.batPct <= 3 && !R.usb && R.batMv && R.batMv < 3450) {
    FL("battery", "empty: " + mvText(R.batMv) + ", switching off");
    flFlush(); brand(L("battery empty", "batterie vide")); push(true); delay(200); pmuOff(); sleepFor(0);
  }
  uint32_t now = nowS();
  if (!R.lastSample || now - R.lastSample >= 1790 || personWake) {
    readRoomNow();
    if (!R.lastSample || now - R.lastSample >= 1790) sampleNow(false);   // v0.3.0: the field log line and the 30-minute record
  }
  if (mayFetch && R.nets) {
    bool due = !R.lastFetch || now - R.lastFetch >= 3600;
    bool retry = !timeValid() && R.fetchFails < 3;               // no clock yet: try a few more times, then hourly
    if (due || retry) goOnline(R.lastFetch ? "hourly" : "first start");
  }
}
void setup() {
  bool cold = coldStart();
  if (cold || R.magic != RTC_MAGIC) {
    memset(&R, 0, sizeof R); R.magic = RTC_MAGIC; R.tile = T_CLOCK; R.scr = SCR_TILE; R.sec = -1; R.roomT10 = R.outT10 = -999; R.roomH = -1; R.outCode = -1; R.batPct = -1;
    for (int i = 0; i < 3; i++) { R.dMax10[i] = R.dMin10[i] = -999; R.dCode[i] = -1; }
    R.kbBat = -1; R.shCard = 255;                                    // v0.3.0
    flMagic = 0; flLen = 0; flRunStartUs = 0;
  }
  boardInit(cold);
  prefs.begin(EK_SET_NAMESPACE, false);
  static EkPrefsKV kvStore(prefs); kv = &kvStore;
  tzCache = prefs.getString(EK_SET_TZ, EK_TZ_DEFAULT); if (!tzCache.length()) tzCache = EK_TZ_DEFAULT;
  setenv("TZ", tzCache.c_str(), 1); tzset();
  if (cold) rtcChipToSystem();
  if (cold && !prefs.isKey("lang")) prefs.putString(EK_SET_LANG, "en");
  R.fr = prefs.getString(EK_SET_LANG, "en") == "fr"; uiFr = R.fr;
  if (!prefs.getBool("wSeeded", false)) {                          // the Classic's own networks, from the build secrets, once
    String s = EK_WIFI_SSID, p = EK_WIFI_PASS;
    while (s.length()) {
      int a = s.indexOf('|'), b = p.indexOf('|');
      String sn = a < 0 ? s : s.substring(0, a), pn = b < 0 ? p : p.substring(0, b);
      if (sn.length()) nets.add(*kv, sn.c_str(), pn.c_str());
      s = a < 0 ? String("") : s.substring(a + 1); p = b < 0 ? String("") : p.substring(b + 1);
    }
    prefs.putBool("wSeeded", true);
  }
  nets.load(*kv); R.nets = nets.count;
  prevFb = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
  R.wakes++;
  if (cold) {
    R.run = prefs.getUInt(EK_SET_JOURNAL_RUN, 0) + 1; prefs.putUInt(EK_SET_JOURNAL_RUN, R.run);
    bool open = prefs.getBool(EK_SET_JOURNAL_OPEN, false);
    struct tm lt; time_t e = time(NULL); bool ok = timeValid(); if (ok) localtime_r(&e, &lt);
    readBattery();
    ekJournalHeader(flEmit, NULL, "einkster397", EK_VERSION, EK_ROLE, devName().c_str(), R.run, ok ? &lt : NULL, resetText(), R.batMv);
    if (open) flRaw("!! the last run had no clean end (power cut or crash): its last lines went with the power");
    prefs.putBool(EK_SET_JOURNAL_OPEN, true);
    FL("start", String("board 397, ") + R.nets + " network(s) known, clock " + (timeValid() ? "set" : "not set"));
    batCold = true;                                                 // v0.3.0: the first sample says it came at a power-on
    brand(EK_VERSION); push(true);
    housekeeping(true, true);
    freshen(); render(); push(true);
    shKbStart();                                                     // v0.4.0: switched on by a person
  } else {
    int pin = wakePin();
    if (R.scr == SCR_TYPE || R.scr == SCR_KB) R.scr = SCR_TYPE;      // v0.2.0: the typewriter draws itself again in full
    else if (pin >= 0 || !shScreen()) restorePrev();                 // the picture now on the paper, drawn again from the RTC state
                                                                     // (v0.3.0: an archive page only when the dial woke it: the card is read)
    if (pin >= 0) {
      FL("wake", pin == PIN_PRESS ? "dial pressed" : pin == PIN_UP ? "dial up" : "dial down");
      housekeeping(true, false);
      // the turn that woke it counts as a move; a press that is still held after the start may be a long one
      int ev = pin == PIN_UP ? EV_UP : pin == PIN_DOWN ? EV_DOWN : EV_PRESS;
      if (pin != PIN_PRESS) { uint32_t t0 = millis(); while (btn(pin) && millis() - t0 < 400) delay(8); }   // the turn that woke it counts once
      if (pin == PIN_PRESS && btn(PIN_PRESS)) { uint32_t t0 = millis(); while (btn(PIN_PRESS) && millis() - t0 < LONG_MS) delay(8); if (btn(PIN_PRESS)) { ev = EV_LONG; while (btn(PIN_PRESS)) delay(8); } }
      if (R.scr == SCR_TYPE) { twEnter(true); if (R.scr != SCR_TYPE) { freshen(); render(); push(true); } }   // v0.2.0: back to writing
      else { act(ev); freshen(); render(); shPush(); if (R.scr != SCR_TYPE) shKbStart(); }   // v0.4.0: a person: listen for the keyboard
    } else {
      housekeeping(false, true);
      if (R.scr == SCR_TILE) {
        freshen();
        bool hourly = timeValid() && (time(NULL) / 60) % 60 == 0;  // the clean refresh at the top of the hour
        render(); push(hourly);
      }
      goSleep();
    }
  }
  lastInput = millis();
}
void loop() {
  if (R.scr == SCR_TYPE) {                       // v0.2.0: the typewriter has the dial and the keyboard to itself
    static uint32_t lp = 0;
    if (millis() - lp > 250) { lp = millis(); if (pwrLongPressed()) { twSessionEnd("ended (switched off)"); switchOff("PWR held"); } }
    twLoop(); delay(2); return;
  }
  shPoll(); sampleIfDue();                       // v0.4.0: keys on every screen, the keyboard coming back, the samples
  if (shScreen()) { if (shDirty) { shDirty = false; render(); shPush(); } else shSettle(); }
  else if (navDirty || shDirty) { navDirty = shDirty = false; if (R.scr != SCR_TYPE) { freshen(); render(); shPush(); } }
  int ev = readDial();
  if (ev != EV_NONE) {
    act(ev);
    freshen(); render(); shPush();
  }
  static uint32_t lastPmu = 0;
  if (millis() - lastPmu > 250) { lastPmu = millis(); if (pwrLongPressed()) switchOff("PWR held"); }
  if (R.scr == SCR_TILE && R.tile == T_CLOCK && timeValid() && epochMin() != R.faceMin) { freshen(); render(); push(false); }
  uint32_t awake = kbConnected() ? (shScreen() ? 600000UL : 120000UL) : shListenUntil ? 65000UL : AWAKE_MS;   // v0.4.0: with the keyboard, longer
  if (millis() - lastInput > awake) {
    kbOff(); kbListenStop(); shListenUntil = 0;                      // the radio sleeps with the 397
    if (shScreen()) { render(); push(false); shClose(); sdEnd(); }   // the page as it will be found again: no keyboard
    FL("sleep", String("after ") + (awake / 1000) + " s without a turn, on " + (R.scr == SCR_TILE ? tileName(R.tile) : "a page"));
    if (R.scr == SCR_TILE) { freshen(); render(); push(false); }
    goSleep();
  }
  delay(10);
}
