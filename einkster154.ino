// EINKSTER 154 · v0.26.1: ask out loud, read on paper.
// Hold the screen, speak, release. The answer settles onto the paper.
// Tap the idle face to switch tiles: ask -> der die das -> weather -> clock -> oracle -> jar -> ask ...
// Clock: ticks every minute, even asleep, without flashing (full refresh once an hour).
// Swipe left = next tile, swipe right = previous tile (from anywhere, answers included).
// A tap that wakes it from sleep just wakes it: same tile, same screen.
// Touch on tiles: swipe = next/previous tile, middle tap = go deeper (if the tile has a deeper level).
// Clock deep dive: alarm and timer, ringing from sleep (FIP live, chime, beacon).
// Kaffeesatz tile: the daily edito from the Kaffeesatz paper, fetched in the morning, read offline.
// Radio tile: FIP live, a 70s pocket receiver. Middle tap: play/stop; left/right: volume.
// Escape hatch: hold PWR 5 s at any time = switch off / restart, even if the firmware is stuck.
// Catalog tile: InvisibleChaos stations as big QR codes, to share a link face to face.
// Offline-first: after a failed connection, tiles show what they have for 10 minutes.
// Editions tile: offline card packs (built in: Atlantid!a Lite, Edit!on Zero; more on microSD). Spec: EDITIONS.md
// Every 30 min a sample goes to flash: battery, room temp/humidity, outdoor temp.
// Climate tile (CLIMATE): 7-day room graph. Battery tile (BATLOG, temporary): drain + days left.
// Oracle: today's line (hold to ask anything). Jar: a question for you (hold to answer).
// Weather draws itself (refreshes every 30 min, even asleep); hold on it to ask about the weather.
// Sleeps 20 s after the last touch. Hold the screen to wake and talk: speak after the beep.
// Tap an answer to go back to the idle face.
// On battery: press PWR to switch on, hold PWR 2 s to switch off.
// Several Wi-Fi networks: put them in the secrets separated by |  (e.g. Home|Hotspot).
// The one that worked last is tried first.
// Cl!cker tile (Classic, v0.25.2): the 154 as a Bluetooth keyboard. Turns pages, scrolls, sends. See ek_hid.h.
// What ended the run (v0.26.1, Classic): a saved crash report, where the run was (asleep, awake, Wi-Fi, sound),
// and lines that reach flash at every sleep after a touch and at least every 2 hours. All in the field log.
// Interface language (v0.26.0): English or French on either edition, chosen on the setup page, stored as "lang".
// Sensors page (v0.25.3): tap the middle of the Device tile. The chips on the I2C bus and the pressure sensor, live.
// Pressure sensor power (v0.25.4): its power wire on header pin GP2 lets the firmware switch it on only to read.
// On the 3V3 pin it still works, always on.
// Night light (v0.25.5): the bottom half of the Device tile switches the sensor module's red LED on and off. It goes off
// by itself 15 minutes after the last touch or button press, in battery reserve, and at power-off.
// Field log (Classic only): what the device did, kept for testing. BOOT button = Mark. Long hold on the Battery tile = all the logs on one page.
// Battery: under 3.55 V it goes into reserve (no background fetching, a reading every 5 min); under 3.51 V it shows
// "battery empty" on the paper and switches itself off, so a dead unit never looks alive.
// Board: Waveshare ESP32-S3-Touch-ePaper-1.54 (V2). InvisibleChaos.

#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "ek_shared.h"             // the parts every E!nkster board shares (tile contract v1)
#include <LittleFS.h>
#include <SD_MMC.h>
#include <functional>
#include "qrcode.h"
#include "mp3dec.h"                   // Helix MP3 decoder, bundled with the ESP32 core (FIP over Icecast)
extern "C" {
#include "src/helix-aac/aacdec.h"         // Helix AAC decoder, fetched by prepare.py (FIP over HLS: no pre-roll ads)
}
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_partition.h"
#include "esp_core_dump.h"              // v0.26.1: the crash report the chip saves on flash
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include <sys/time.h>
#include <time.h>
#include "esp_sntp.h"
#include "user_config.h"
#include "secrets.h"            // written by prepare.py from config.h or the environment
// ---- build profile: Classic (default) or InvisibleOcean (French UI, offline-first tile set, no secrets)
#ifndef EK_INVISIBLEOCEAN
#define EK_INVISIBLEOCEAN 0
#endif
// v0.26.0: the interface language is a setting (EK_SET_LANG, "lang": "en" or "fr"), read at every start. Each edition
// keeps its own default: English on the Classic, French on the InvisibleOcean. Before, it was fixed when the firmware was built.
static bool uiFr = EK_INVISIBLEOCEAN;
#define EK_FR (uiFr)
#define L(en, fr) (EK_FR ? (fr) : (en))            // UI text in the interface language
#include "fonts.h"
#include "audio.h"
#include "src/codec_board/codec_init.h"
#include "src/esp_codec_dev/include/esp_codec_dev.h"
#define private public                // reach the panel's own sleep command (0x10)
#include "src/display/epaper_driver_bsp.h"
#undef private
#include "src/power/board_power_bsp.h"
#include "src/i2c_bsp/i2c_bsp.h"

struct FipStream;                          // radio stream (declared early: generated prototypes use it)
// ringer state (declared early for the same reason)
struct Synth { uint32_t n = 0; float lpA = 0, lpB = 0, ph = 0; };
// one log sample (declared early: the Arduino build generates prototypes that use it)
struct BatSample {                             // 16 bytes (v2; the 12-byte v1 file is migrated once)
  uint32_t t; uint16_t mv; uint8_t wifi, asks;
  int16_t tin10;                               // room temp x10, INT16_MIN = none
  uint8_t rh;                                  // room humidity %, 255 = none
  int8_t tout;                                 // outdoor temp (fresh forecast only), -128 = none
  uint16_t p10;                                // local pressure at sea level x10 (BMP388), 0 = no sensor
  uint16_t spare;                              // flags since v0.23.2 (0 before): LOGF_COLD, LOGF_RESERVE
};
#define LOGF_COLD    1                         // read at a cold start: the battery reads about 50 mV low
#define LOGF_RESERVE 2                         // taken in reserve, at the 5-minute rhythm
struct BatSampleV1 { uint32_t t; uint16_t mv; uint8_t wifi, asks; int16_t tin10; uint8_t rh; int8_t tout; };

#define EK_VERSION   "v0.26.2"
#define MAX_SECONDS  10
#define MIN_MS       600         // shorter holds are treated as taps
#define HOLD_MS      250         // press longer than this = talk
#define GHOST_CLEAN  10          // going to sleep after use with this many partials: one clean full refresh
#define GHOST_CAP    40          // during long use: a full refresh after this many partials
#define PWR_BTN      GPIO_NUM_18 // side button, reads LOW when pressed
#define OFF_MS       2000        // hold PWR this long to switch off
#define TOUCH_INT    GPIO_NUM_21 // touch chip interrupt: LOW while a finger is down
#define AWAKE_MS     20000       // stay awake this long after the last touch
#define SILENT_PEAK  500         // recordings quieter than this never leave the device
#define SWIPE_PX     40          // finger travel that turns a touch into a swipe
#define SWIPE_FLIP   0           // set to 1 if swipes go the wrong way on this panel
#define BATLOG       (!EK_INVISIBLEOCEAN)   // TEMPORARY battery tile (Classic only)
#define CLIMATE      1           // Clim@te tile: weather station with pages (always on)
#define EK_LOG       (BATLOG || CLIMATE)   // the 30-minute sampler both tiles read from
#define CLICKER      (!EK_INVISIBLEOCEAN)   // v0.25.2: the Cl!cker tile, keyboard mode (Classic only for now)
#define LOG_EVERY_S  (30UL * 60)
#define EK_TZ        "CET-1CEST,M3.5.0,M10.5.0/3"   // Berlin, with summer time
#define NTP_EVERY_S  (6UL * 60 * 60)                // re-check internet time this often
#define BAT_PIN      4           // battery voltage through a 1:2 divider

static epaper_driver_display *epd = NULL;
static board_power_bsp_t power(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
static uint8_t *rec = NULL;                     // stereo capture buffer (PSRAM)
// alarm and timer settings (the engine is further down)
enum { TONE_FIP, TONE_CHIME, TONE_BEACON, TONE_COUNT };
static int alH = 7, alM = 30, alRep = 2, alTone = TONE_CHIME, tmSecs = 300, tmTone = TONE_CHIME;
static bool alOn = false;
RTC_DATA_ATTR static uint32_t rtcSnoozeAt, rtcTimerEnd;
static const uint32_t REC_MAX = EK_SAMPLE_RATE * 4 * MAX_SECONDS;
static Preferences prefs;

// ---------------------------------------------------------------- tiles
enum { T_ASK, T_DERDIEDAS, T_ORACLE, T_JAR, T_KAFFEE, T_RADIO, T_WEATHER,   // online tiles
       T_CLOCK,                                                // offline from here on
#if CLIMATE
       T_CLIMATE,
#endif
       T_EDITIONS, T_CATALOG,
#if BATLOG
       T_BATTERY,
#endif
       T_DEVICE,                                               // v0.25.0: fixed, always the last tile, in no recipe
#if CLICKER
       T_CLICKER,                                              // v0.25.2: after the device in the list, so stored tiles keep their number
#endif
       T_COUNT };
#if !CLIMATE
#define T_CLIMATE 98
#endif
#if !BATLOG
#define T_BATTERY 99
#endif
#if !CLICKER
#define T_CLICKER 97
#endif
// the edition's tiles, in swipe order (tiles not listed do not exist in that edition)
#if EK_INVISIBLEOCEAN
static const uint8_t TILES[] = { T_EDITIONS, T_CLOCK, T_CLIMATE, T_RADIO, T_CATALOG, T_DEVICE };
#else
static const uint8_t TILES[] = { T_ASK, T_DERDIEDAS, T_ORACLE, T_JAR, T_KAFFEE, T_RADIO, T_CLIMATE, T_CLOCK,
  T_EDITIONS, T_CATALOG,
#if CLICKER
  T_CLICKER,
#endif
#if BATLOG
  T_BATTERY,
#endif
  T_DEVICE,
};
#endif
#define N_TILES (int)sizeof(TILES)
static int tilePos(int t) { for (int i = 0; i < N_TILES; i++) if (TILES[i] == t) return i; return -1; }
// indexed by tile; the optional tiles sit where the enum puts them
static const char *tileMode(int t) {
  return t == T_DERDIEDAS ? "derdiedas" : t == T_WEATHER ? "weather" : t == T_ORACLE ? "oracle"
       : t == T_JAR ? "jar" : "ask";
}
static const char *tileHint(int t) {
  return t == T_ASK ? "hold to talk" : t == T_DERDIEDAS ? "hold, say a noun" : "";
}
static const char *tileWait(int t) {
  return t == T_WEATHER ? "weather..." : t == T_ORACLE ? "consulting..." : t == T_JAR ? "shaking the jar..." : "";
}
#define ORACLE_STALE_S (3UL * 60 * 60)        // re-check for a new day's line
#define JAR_STALE_S    (12UL * 60 * 60)       // an unanswered question keeps this long
#define WX_STALE_S     (30UL * 60)            // weather face refreshes after this
static String orBody, jarBody;                // last replies from the brain
static uint32_t orAt = 0, jarAt = 0;
// ---- the weather, parsed once from Open-Meteo and kept through deep sleep (see the CLIM@TE section)
#if EK_INVISIBLEOCEAN
#define EK_LAT "49.6337"                       // Cherbourg
#define EK_LON "-1.6222"
#define EK_ALT_M 10
#define EK_WX_TOWN "Cherbourg"                 // v0.26.0: the town the online weather is for, named on the Sensors page
#define EK_TIDE_DATUM 3.85f                    // chart datum (zéro hydrographique) below mean sea level, metres: check once against SHOM
#else
#define EK_LAT "52.5200"                       // Berlin
#define EK_LON "13.4050"
#define EK_ALT_M 40
#define EK_WX_TOWN "Berlin"
#define EK_TIDE_DATUM 0.0f
#endif
// v0.26.0: the device's height above sea level, metres, for the sensor's sea-level value. The edition's figure until one
// tap on the Sensors page matches the sensor to the online reading; then the setting EK_SET_ALT ("alt"). Personal like
// lat and lon: never in the journal.
static int altM = EK_ALT_M;
struct WxData {
  uint32_t at; bool valid;
  float t, rh, feels, wind, gust, pmsl, uv; int code, day, wdir;
  float rain15[8];                             // next two hours, mm per quarter hour
  uint8_t hh[12], hp[12], hcode[12], hwind[12]; float ht[12];   // the next twelve hours
  int16_t pres[96];                            // sea-level pressure x10, hourly: 60 h back .. 36 h ahead (0 = none)
  float dmax[5], dmin[5], dprec[5], duv[5]; uint8_t dcode[5]; int16_t sunrise[5], sunset[5];
  bool marine, noMarine; float wave, wperiod, sst; int wavedir; int16_t sea[192]; uint32_t seaT0;   // 48 h of sea level, cm, per quarter hour
  bool air; int aqi; float pm25, pm10, o3, no2; uint8_t pollen[6];
  float pLocal, pLocal3h;                      // the fitted sensor, reduced to sea level (0 = no sensor)
};
RTC_DATA_ATTR static WxData wx;
static bool jarAnswered = false;
static int tile = TILES[0];
static bool onIdle = true;
static bool needFull = true;                  // next draw must be a full refresh (after boot/wake)
RTC_DATA_ATTR static uint16_t rtcGhost = 0;   // partial refreshes since the last full one (kept through sleep)
static bool userSession = false;             // awake because someone is using it (not a timer tick)
static bool climOn = false;                  // InvisibleOcean: the climate graphs, the weather station's deep level
static uint32_t lastTouch = 0;

// seconds since power-on; keeps counting through deep sleep
static uint32_t nowS() { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec; }

// ---------------------------------------------------------------- memory that survives deep sleep
#define RTC_MAGIC 0xE1C57E05
RTC_DATA_ATTR static uint32_t rtcMagic;
RTC_DATA_ATTR static int8_t   rtcTile, rtcIdle, rtcJarAnswered, rtcSpurious, rtcTouchWakeOff;
RTC_DATA_ATTR static uint32_t rtcOrAt, rtcJarAt, rtcSleptAt;
RTC_DATA_ATTR static char     rtcOr[600], rtcJar[600], rtcLast[600];
RTC_DATA_ATTR static int8_t   rtcLastGrammar, rtcPaperKept;
RTC_DATA_ATTR static uint32_t rtcNtpAt, rtcNetTryAt;
RTC_DATA_ATTR static uint32_t rtcNetDownUntil;      // after a failed connection: stay offline until then
#define NET_BACKOFF_S 600
static bool offlineNow() { return nowS() < rtcNetDownUntil; }
static float insideC = NAN, insideRH = NAN;    // read right after waking, before we warm up
static int wakeMv = 0;                         // battery, read before anything loads it

// ================================================================ FIELD LOG (test devices: Classic only)
// What the device did, one line per event, so that a field test comes back as data and not as an impression.
// Same shape as fieldlog.js in the InvisibleChaos web apps: a header block that starts with FIELD LOG, then
//   T+mm:ss.s  HH:MM:SS.mmm  [type] detail
// T+ counts from the start of the run (power-on to power-off) and keeps counting through deep sleep.
// Lines wait in memory that survives deep sleep and a crash restart. They go to flash when that memory fills,
// at power-off, at the next start after a crash, and when the log is opened. Two files of 40 kB each: when both
// are full the older one goes, so the newest lines are always kept.
// A power cut loses the lines still in memory (at most FL_RING bytes). The next start says so in the log.
// Nothing leaves the device unless the log is opened by hand: a long hold on the Battery tile.
// The log holds no network name, no password, no key, no question and no answer.
// BOOT button = Mark: "MARK n" lands in the log at that second.
// InvisibleOcean: compiled out. School units log nothing.
#define EK_FIELDLOG (!EK_INVISIBLEOCEAN)
#define FL(type, detail) do { if (EK_FIELDLOG) flEvent((type), (detail)); } while (0)
// v0.26.1: on battery, any restart (a crash, a watchdog, a voltage dip) lets go of the battery latch, so it looks exactly
// like a power cut: the next start reads "power on". Two clues tell them apart. (1) A crash leaves a report on flash,
// read and logged at the next start, then erased. (2) "wkStep" in the stored settings says where the run was when it
// ended: 1 asleep, 2 awake, 3 Wi-Fi started in this wake, 4 sound started, 0 a clean end. Written only when it changes.
static uint8_t wkNow = 255, wkBoot = 0;          // wkBoot: as the last run left it, read first thing at start
static void wk(uint8_t st) {
#if EK_FIELDLOG
  if (st == wkNow) return;
  wkNow = st; prefs.putUChar("wkStep", st);
#endif
}
static String flSecs(uint32_t ms) { return String(ms / 1000.0f, 1) + " s"; }
static String flHost(const String &url) {      // the host of an address, never its query (coordinates stay out of the log)
  int a = url.indexOf("//"); a = a < 0 ? 0 : a + 2;
  int b = url.indexOf('/', a); return url.substring(a, b < 0 ? url.length() : b);
}
#if EK_FIELDLOG
extern "C" uint64_t esp_rtc_get_time_us(void);  // the chip's own clock: counts through deep sleep and restarts, zero at power-on
#define FL_MAGIC 0xF1E7D106
#define FL_RING  2000
#define FL_CAP   (40 * 1024)
RTC_NOINIT_ATTR static uint32_t flMagic;       // says the ring below is ours (it is garbage after a power cut)
RTC_NOINIT_ATTR static uint32_t flRun;         // the run in progress, 0 = none
RTC_NOINIT_ATTR static uint64_t flT0;          // the clock above, when the run began
RTC_NOINIT_ATTR static uint16_t flLen, flLost, flMarks;
RTC_NOINIT_ATTR static uint8_t  flSens;        // pressure sensor: 0 not looked yet, 1 absent, 2 answers
RTC_NOINIT_ATTR static char     flBuf[FL_RING];
static bool flWakeNoted = false;               // this wake already has its [wake] line
static const char *flWakePending = NULL;       // a quiet timer wake: named only if something happens in it
static bool flRingOk() { return flMagic == FL_MAGIC && flLen <= FL_RING; }
static void flFlush() {                        // the waiting lines go to flash
  if (!flRingOk() || !flLen || !lfsMount()) return;
  File f = LittleFS.open("/field.txt", "a");
  if (!f) return;
  if (f.size() + flLen > FL_CAP) {             // full: the older file goes, this one becomes the older one
    f.close();
    if (LittleFS.exists("/field.old")) {
      File o = LittleFS.open("/field.old", "r"); uint32_t n = 0;
      if (o) { static uint8_t b[256]; int k; while ((k = o.read(b, sizeof b)) > 0) for (int i = 0; i < k; i++) if (b[i] == '\n') n++; o.close(); }
      prefs.putUInt("flDrop", prefs.getUInt("flDrop", 0) + n);   // the export says how many lines went
      LittleFS.remove("/field.old");
    }
    LittleFS.rename("/field.txt", "/field.old");
    f = LittleFS.open("/field.txt", "a");
    if (!f) return;
  }
  size_t w = f.write((const uint8_t *)flBuf, flLen); f.close();
  if (w == flLen) flLen = 0;
}
static void flRaw(const char *s) {             // one line into the ring
  size_t n = strlen(s); if (n > 220) n = 220;
  if (flLen + n + 1 > FL_RING) flFlush();
  if (flLen + n + 1 > FL_RING) { if (flLost < 65535) flLost++; return; }   // flash refused: count it, keep what we hold
  memcpy(flBuf + flLen, s, n); flLen += n; flBuf[flLen++] = '\n';
}
static void flLine(const char *type, const String &detail) {
  uint32_t ds = (uint32_t)((esp_rtc_get_time_us() - flT0) / 100000ULL);    // tenths of a second since the run began
  char clock[16] = "--:--:--.---";                                          // the clock is not set yet
  if (timeValid()) {
    struct timeval tv; gettimeofday(&tv, NULL); time_t e = tv.tv_sec; struct tm t; localtime_r(&e, &t);
    snprintf(clock, sizeof clock, "%02d:%02d:%02d.%03d", t.tm_hour, t.tm_min, t.tm_sec, (int)(tv.tv_usec / 1000));
  }
  char line[224];
  snprintf(line, sizeof line, "T+%02lu:%02lu.%lu  %s  [%s] %s", (unsigned long)(ds / 600), (unsigned long)(ds / 10 % 60),
           (unsigned long)(ds % 10), clock, type, detail.c_str());
  flRaw(line);
}
static void flStartRun(const char *reset) {    // the header block of a new run
  if (!flRingOk()) { flMagic = FL_MAGIC; flLen = 0; flLost = 0; }
  flRun = prefs.getUInt("flRun", 0) + 1; prefs.putUInt("flRun", flRun); prefs.putBool("flOpen", true);
  flT0 = esp_rtc_get_time_us(); flMarks = 0; flSens = 0;
  uint64_t mac = ESP.getEfuseMac(); char b[96];
  flRaw("");                                   // a blank line between runs
  flRaw("FIELD LOG");
  flRaw("app       einkster154");
  flRaw("version   " EK_VERSION);
  flRaw("role      classic");
  snprintf(b, sizeof b, "device    E!nkster-%02X%02X", (int)((mac >> 32) & 0xFF), (int)((mac >> 40) & 0xFF)); flRaw(b);
  snprintf(b, sizeof b, "run       %lu", (unsigned long)flRun); flRaw(b);
  if (timeValid()) { time_t e = time(NULL); struct tm t; localtime_r(&e, &t); strftime(b, sizeof b, "started   %Y-%m-%d %H:%M:%S", &t); }
  else strcpy(b, "started   clock not set");
  flRaw(b);
  snprintf(b, sizeof b, "reset     %s", reset); flRaw(b);
  if (wakeMv >= 2800) snprintf(b, sizeof b, "battery   %d.%02d V", wakeMv / 1000, wakeMv % 1000 / 10); else strcpy(b, "battery   no cell seen");
  flRaw(b);
  if (flLost) { snprintf(b, sizeof b, "note      %u lines lost before this run: flash refused them", (unsigned)flLost); flRaw(b); flLost = 0; }
}
static void flEvent(const char *type, const String &detail) {
  if (!flRingOk() || !flRun) flStartRun("log goes on (still powered after the last end)");
  if (flWakePending) { const char *w = flWakePending; flWakePending = NULL; flWakeNoted = true; flLine("wake", w); }
  flLine(type, detail);
}
static void flEnd(const char *reason) {        // a clean end: the run is closed and on flash
  wk(0);                                       // v0.26.1
  if (!flRingOk() || !flRun) return;
  flLine("end", reason); flFlush();
  prefs.putBool("flOpen", false); flRun = 0;
}
static void flSensor(bool present) {           // said once per run, and again if it changes
  uint8_t v = present ? 2 : 1;
  if (!flRingOk() || !flRun || flSens == v) return;
  flSens = v; flEvent("sensor", present ? "pressure sensor answers" : "no pressure sensor on the header");
}
#else
static void flEvent(const char *, const String &) {}
static void flEnd(const char *) {}
static void flSensor(bool) {}
#endif

// ---------------------------------------------------------------- TEMPORARY battery log
RTC_DATA_ATTR static BatSample rtcBat[16];
RTC_DATA_ATTR static uint8_t  rtcBatN, rtcWifiN, rtcAskN;
RTC_DATA_ATTR static uint32_t rtcBatAt;
RTC_DATA_ATTR static uint32_t rtcFlAt;          // v0.26.1: when the field log last reached flash
static bool wifiCounted = false;
static String lastAnswer;                     // what an answer screen shows, to restore it
static bool lastGrammar = false;
static void keep(char *dst, size_t cap, const String &s, const char *from) {
  int i = from ? s.indexOf(from) : 0;         // weather: only the payload is needed
  if (i < 0) i = 0;
  if (s.length() - i < cap) strcpy(dst, s.c_str() + i); else dst[0] = 0;
}

// ---------------------------------------------------------------- drawing
static void px(int x, int y, bool ink) {
  if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
  epd->EPD_DrawColorPixel(x, y, ink ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE);
}
static void rect(int x, int y, int w, int h, bool ink) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, ink);
}
static void disc(int cx, int cy, int r, bool ink = true) {
  for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++)
    if (x * x + y * y <= r * r) px(cx + x, cy + y, ink);
}
static void ring(int cx, int cy, int r, int t, bool lowerHalfOnly) {
  for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {
    int d = x * x + y * y;
    if (d <= r * r && d >= (r - t) * (r - t) && (!lowerHalfOnly || y >= 0))
      px(cx + x, cy + y, true);
  }
}
static void micIcon(int cx, int cy) {
  rect(cx - 11, cy - 30, 22, 36, true);          // capsule body
  disc(cx, cy - 30, 11, true);
  disc(cx, cy + 6, 11, true);
  ring(cx, cy + 4, 24, 5, true);                  // cradle
  rect(cx - 2, cy + 27, 5, 12, true);             // stem
  rect(cx - 14, cy + 38, 29, 5, true);            // base
}

// UTF-8 -> codepoints (BMP). Anything the fonts lack is drawn as a box, never dropped.
static int decodeUtf8c(const char *c, uint16_t *out, int max) {
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
static int decodeUtf8(const String &s, uint16_t *out, int max) { return decodeUtf8c(s.c_str(), out, max); }

static int glyphW(const EkFont &f, uint16_t cp) {
  if (cp == '\n') return 0;
  int i = ek_glyph_index(cp); return i < 0 ? f.height / 2 + 3 : f.glyphs[i].width;
}
static void drawGlyph(const EkFont &f, int x, int y, uint16_t cp) {
  int i = ek_glyph_index(cp);
  if (i < 0) {                              // missing glyph: hollow box
    if (cp == '\n') return;
    int w = glyphW(f, cp);
    rect(x + 1, y + 3, w - 3, 1, true); rect(x + 1, y + f.height - 5, w - 3, 1, true);
    rect(x + 1, y + 3, 1, f.height - 7, true); rect(x + w - 3, y + 3, 1, f.height - 7, true);
    return;
  }
  const EkGlyph &g = f.glyphs[i]; int bpr = (g.width + 7) / 8;
  const uint8_t *b = f.bits + g.offset;
  for (int r = 0; r < f.height; r++) for (int c = 0; c < g.width; c++)
    if (b[r * bpr + (c >> 3)] & (0x80 >> (c & 7))) px(x + c, y + r, true);
}

// word-wrap into lines; returns line count, or -1 if it does not fit
#define MAXL 16
static int layout(const EkFont &f, const uint16_t *t, int n, int w, int h,
                  int starts[], int ends[]) {
  int lines = 0, i = 0, maxLines = h / f.height;
  while (i < n && lines < MAXL) {
    while (i < n && t[i] == ' ') i++;
    int s = i, lastSpace = -1, x = 0;
    while (i < n && t[i] != '\n') {
      int gw = glyphW(f, t[i]);
      if (x + gw > w) break;
      if (t[i] == ' ') lastSpace = i;
      x += gw; i++;
    }
    int e = i;
    if (i < n && t[i] != '\n' && lastSpace > s) { e = lastSpace; i = lastSpace + 1; }
    if (i < n && t[i] == '\n') i++;
    starts[lines] = s; ends[lines] = e; lines++;
  }
  if (i < n || lines > maxLines) return -1;
  return lines;
}

// draw text block centered in the box, picking the largest font that fits
static void textBlock(const String &s, int bx, int by, int bw, int bh, bool preferBig) {
  static uint16_t t[1200];
  int n = decodeUtf8(s, t, 1200);
  const EkFont *fonts[3] = { &EK_BIG, &EK_MID, &EK_SMALL };
  int starts[MAXL], ends[MAXL], lines = -1, k = preferBig ? 0 : 1;
  for (; k < 3; k++) { lines = layout(*fonts[k], t, n, bw, bh, starts, ends); if (lines > 0) break; }
  if (lines <= 0) {           // still too long: fill with the small font and cut
    k = 2; const EkFont &f = *fonts[k];
    int maxLines = bh / f.height, cut = n;
    while (cut > 0 && layout(f, t, cut, bw, bh, starts, ends) < 0) cut -= 4;
    t[cut > 3 ? cut - 3 : 0] = '.'; t[cut > 2 ? cut - 2 : 1] = '.'; t[cut > 1 ? cut - 1 : 2] = '.';
    lines = layout(f, t, cut, bw, bh, starts, ends);
    if (lines > maxLines) lines = maxLines;
  }
  const EkFont &f = *fonts[k];
  int y = by + (bh - lines * f.height) / 2;
  for (int l = 0; l < lines; l++) {
    int w = 0; for (int i = starts[l]; i < ends[l]; i++) w += glyphW(f, t[i]);
    int x = bx + (bw - w) / 2;
    for (int i = starts[l]; i < ends[l]; i++) { drawGlyph(f, x, y, t[i]); x += glyphW(f, t[i]); }
    y += f.height;
  }
}

static bool partialOnly = false;           // countdowns: no routine full refresh
// One refresh policy for every tile: partial updates by default; clean full refreshes only when
// needed (after boot or a cold panel), when going to sleep after use, or after a long session.
static void show() {                       // push the buffer to the paper
  if (needFull || (!partialOnly && rtcGhost >= GHOST_CAP)) {
    needFull = false; rtcGhost = 0;
    epd->EPD_Init(); epd->EPD_DisplayPartBaseImage(); epd->EPD_Init_Partial();
  } else {
    epd->EPD_DisplayPart(); if (rtcGhost < 65000) rtcGhost++;
  }
}

// one line of text in a given font, centered on cx
static int textWidth(const EkFont &f, const String &s) {
  static uint16_t t[64]; int n = decodeUtf8(s, t, 64), w = 0;
  for (int i = 0; i < n; i++) w += glyphW(f, t[i]);
  return w;
}
static void textAt(const EkFont &f, int cx, int y, const String &s) {
  static uint16_t t[64]; int n = decodeUtf8(s, t, 64);
  int x = cx - textWidth(f, s) / 2;
  for (int i = 0; i < n; i++) { drawGlyph(f, x, y, t[i]); x += glyphW(f, t[i]); }
}

// ---------------------------------------------------------------- weather drawing
static void cloud(int cx, int cy) {
  disc(cx - 13, cy + 3, 10); disc(cx + 1, cy - 5, 14); disc(cx + 14, cy + 4, 10);
  rect(cx - 13, cy + 3, 27, 11, true);
}
static void sun(int cx, int cy, int r) {
  disc(cx, cy, r);
  for (int a = 0; a < 8; a++) {                // eight short rays
    float ang = a * PI / 4;
    for (int k = r + 4; k <= r + 9; k++) {
      int x = cx + (int)(cosf(ang) * k), y = cy + (int)(sinf(ang) * k);
      rect(x - 1, y - 1, 3, 3, true);
    }
  }
}
static void moon(int cx, int cy, int r) {     // crescent: a disc bitten by a white disc
  disc(cx, cy, r);
  disc(cx + r * 5 / 8, cy - r * 3 / 8, r * 7 / 8, false);
}
static void star(int x, int y) { rect(x - 3, y, 7, 1, true); rect(x, y - 3, 1, 7, true); }
static void wxIcon(int code, int cx, int cy, bool day) {
  if (code == 0) {
    if (day) sun(cx, cy, 13);
    else { moon(cx - 4, cy, 17); star(cx + 20, cy - 12); star(cx + 14, cy + 14); }
    return;
  }
  if (code <= 2) {
    if (day) sun(cx - 9, cy - 9, 9); else moon(cx - 10, cy - 10, 12);
    rect(cx - 30, cy - 4, 60, 30, false); cloud(cx + 4, cy + 8); return;
  }
  if (code == 45 || code == 48) {              // fog: stacked bars
    for (int i = 0; i < 4; i++) rect(cx - 26 + (i % 2) * 6, cy - 14 + i * 9, 46, 4, true);
    return;
  }
  cloud(cx, cy - 6);
  if (code == 3) return;
  bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  if (code >= 95) {                            // thunder: a bolt
    for (int j = 0; j < 9; j++) rect(cx + 2 - j / 2, cy + 12 + j, 4, 1, true);
    rect(cx - 3, cy + 20, 8, 3, true);
    for (int j = 0; j < 9; j++) rect(cx - 1 - j / 2, cy + 23 + j, 4, 1, true);
    return;
  }
  for (int i = -1; i <= 1; i++) {              // rain streaks or snow dots
    int x = cx + i * 12;
    if (snow) { disc(x, cy + 17, 3); disc(x + 5, cy + 27, 3); }
    else for (int j = 0; j < 12; j++) px(x - j / 3, cy + 14 + j, true), px(x + 1 - j / 3, cy + 14 + j, true);
  }
}

// tiny JSON readers for the weather payload (numbers only, "null" -> NAN)
static float jnum(const String &b, const char *key, int from) {
  String k = String("\"") + key + "\":";
  int i = b.indexOf(k, from); if (i < 0) return NAN;
  i += k.length();
  if (b.startsWith("null", i)) return NAN;
  return strtof(b.c_str() + i, NULL);
}


static String wxFetch();                    // the weather payload: brain (Classic) or Open-Meteo (InvisibleOcean)
static bool wifiConfigured();               // InvisibleOcean: a network was set up on the device. Classic: always (it is in the build)
static String callBrain(const char *mode, const char *ctype, const uint8_t *data, size_t len);
static String callBrainF(const char *mode, const char *ctype, const uint8_t *data, size_t len, bool force);
static void tileDots();
static void show();

static void sparkle(int cx, int cy) {           // the oracle's mark
  for (int k = 0; k <= 9; k++) { int w = k < 3 ? 3 : 1;
    rect(cx - w / 2, cy - 9 + k, w, 1, true); rect(cx - w / 2, cy + 9 - k, w, 1, true);
    rect(cx - 9 + k, cy - w / 2, 1, w, true); rect(cx + 9 - k, cy - w / 2, 1, w, true); }
  disc(cx, cy, 2);
}
static void drawOracle(const String &b) {
  String line = jsonField(b, "answer");
  if (!line.length()) { textBlock("the oracle is silent.\n" + jsonField(b, "error"), 8, 20, 184, 170, false); return; }
  sparkle(100, 30);
  textBlock(line, 10, 44, 180, 124, true);
  String d = jsonField(b, "date");
  if (d.length() == 10) textAt(EK_SMALL, 100, 178, d.substring(8) + "." + d.substring(5, 7) + ".");
}
static void jarIcon(int cx, int cy) {           // a small outlined jar
  rect(cx - 7, cy - 12, 14, 3, true);           // lid
  rect(cx - 9, cy - 8, 18, 1, true); rect(cx - 9, cy + 9, 18, 1, true);
  rect(cx - 9, cy - 8, 1, 18, true); rect(cx + 8, cy - 8, 1, 18, true);
  rect(cx - 4, cy - 1, 8, 1, true); rect(cx - 4, cy + 3, 8, 1, true);   // folded notes
}
static void drawJar(const String &b) {
  String q = jsonField(b, "answer");
  if (!q.length()) { textBlock("the jar is stuck.\n" + jsonField(b, "error"), 8, 20, 184, 170, false); return; }
  jarIcon(100, 32);
  textBlock(q, 10, 48, 180, 124, true);
  textAt(EK_SMALL, 100, 178, "hold to answer");
}
static void fetchFace() {                     // middle tap on Weather, Oracle or Jar: fetch now, then show
  epd->EPD_Clear(); tileDots();
  textBlock(tileWait(tile), 8, 60, 184, 80, false); show();
  epd->EPD_Clear(); tileDots();
  {
    String b = callBrain(tileMode(tile), "application/json", (const uint8_t *)"{\"text\":\"\"}", 11);
    if (tile == T_ORACLE) { if (jsonField(b, "answer").length()) { orBody = b; orAt = nowS(); } drawOracle(jsonField(b, "answer").length() ? b : (orBody.length() ? orBody : b)); }
    else { if (jsonField(b, "answer").length()) { jarBody = b; jarAt = nowS(); jarAnswered = false; } drawJar(jsonField(b, "answer").length() ? b : (jarBody.length() ? jarBody : b)); }
  }
  show(); onIdle = true;
}

// ---------------------------------------------------------------- indoor sensor (SHTC3)
static float magnus(float t) {             // water vapour pressure at saturation, hPa (Magnus formula)
  return 6.112f * expf(17.62f * t / (243.12f + t));
}
static void readInside() {
  uint8_t cmd[2], d[6];
  cmd[0] = 0x35; cmd[1] = 0x17; if (i2c_write_buff(shtc3_handle, -1, cmd, 2) != 0) return;   // wake
  delay(1);
  cmd[0] = 0x78; cmd[1] = 0x66; if (i2c_write_buff(shtc3_handle, -1, cmd, 2) != 0) return;   // measure T first
  delay(15);
  if (i2c_read_buff(shtc3_handle, -1, d, 6) != 0) return;
  cmd[0] = 0xB0; cmd[1] = 0x98; i2c_write_buff(shtc3_handle, -1, cmd, 2);                     // back to sleep
  insideC  = -45.0f + 175.0f * ((d[0] << 8) | d[1]) / 65536.0f;
  insideRH = 100.0f * ((d[3] << 8) | d[4]) / 65536.0f;
  // v0.25.1: the case warms the sensor a little. The correction is a setting in tenths of a degree, default -1.0,
  // from Julien's outdoor check of 14.10.2026. The humidity is moved to the corrected temperature with it.
  float off = prefs.getInt(EK_SET_TEMP_OFF, EK_TEMP_OFF_DEFAULT) / 10.0f;
  if (off != 0) {
    float t = insideC + off, rh = insideRH * magnus(insideC) / magnus(t);
    insideC = t; insideRH = rh < 0 ? 0 : rh > 100 ? 100 : rh;
  }
}

// ---------------------------------------------------------------- time: RTC chip + internet time
static uint8_t bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static uint8_t tobcd(int v) { return ((v / 10) << 4) | (v % 10); }
static bool timeValid() { return time(NULL) > 1735689600; }          // after 2025-01-01
// v0.25.0: the time zone is a setting (EK_SET_TZ), set from the phone on the setup page. Default: Central Europe.
static String tzCache;
static const char *tzRule() {
  if (!tzCache.length()) tzCache = prefs.getString(EK_SET_TZ, EK_TZ);
  if (!tzCache.length()) tzCache = EK_TZ;
  return tzCache.c_str();
}
static void setTzRule(const String &r) { tzCache = r; prefs.putString(EK_SET_TZ, r); setenv("TZ", tzRule(), 1); tzset(); }
static void rtcChipToSystem() {             // the chip keeps UTC through sleep and power-off
  uint8_t r[7];
  if (i2c_read_buff(rtc_dev_handle, 0x04, r, 7) != 0) return;
  if (r[0] & 0x80) return;                  // oscillator stopped: the chip's time is not valid
  struct tm t = {};
  t.tm_sec = bcd(r[0] & 0x7F); t.tm_min = bcd(r[1] & 0x7F); t.tm_hour = bcd(r[2] & 0x3F);
  t.tm_mday = bcd(r[3] & 0x3F); t.tm_mon = bcd(r[5] & 0x1F) - 1; t.tm_year = bcd(r[6]) + 100;
  setenv("TZ", "UTC0", 1); tzset();
  time_t e = mktime(&t);
  setenv("TZ", tzRule(), 1); tzset();
  if (e > 1735689600) { struct timeval tv = { e, 0 }; settimeofday(&tv, NULL); }
}
static void systemToRtcChip() {
  time_t e = time(NULL); struct tm t; gmtime_r(&e, &t);
  uint8_t c = 0x00; i2c_write_buff(rtc_dev_handle, 0x00, &c, 1);    // running, 24 h
  uint8_t r[7] = { tobcd(t.tm_sec), tobcd(t.tm_min), tobcd(t.tm_hour), tobcd(t.tm_mday),
                   (uint8_t)t.tm_wday, tobcd(t.tm_mon + 1), tobcd(t.tm_year - 100) };
  i2c_write_buff(rtc_dev_handle, 0x04, r, 7);
}
static volatile bool ntpFresh = false;      // set by the time service whenever it lands
static void onNtp(struct timeval *) { ntpFresh = true; }
static void commitNtp() {                   // store fresh internet time in the clock chip
  if (!ntpFresh || !timeValid()) return;
  ntpFresh = false;
  systemToRtcChip(); rtcNtpAt = time(NULL);
  FL("time", "clock set from the internet");
}
static void maybeSyncTime() {               // called with Wi-Fi up
  time_t now = time(NULL);
  if (timeValid() && now >= (time_t)rtcNtpAt && now - rtcNtpAt < NTP_EVERY_S) return;
  sntp_set_time_sync_notification_cb(onNtp);
  configTzTime(tzRule(), "ptbtime1.ptb.de", "ptbtime2.ptb.de", "de.pool.ntp.org");
  uint32_t t0 = millis();
  while (!ntpFresh && millis() - t0 < 6000) delay(50);
  commitNtp();                              // if it lands later, loop() or goSleep() commits it
}

// ---------------------------------------------------------------- big clock digits (segments)
static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
static void digit(int x, int y, int d, int w, int h, int k) {   // k = stroke
  uint8_t m = SEG[d]; int hh = h / 2;
  if (m & 0x01) rect(x + k / 2, y, w - k, k, true);                 // a
  if (m & 0x02) rect(x + w - k, y + k / 2, k, hh - k / 2, true);    // b
  if (m & 0x04) rect(x + w - k, y + hh, k, hh - k / 2, true);       // c
  if (m & 0x08) rect(x + k / 2, y + h - k, w - k, k, true);         // d
  if (m & 0x10) rect(x, y + hh, k, hh - k / 2, true);               // e
  if (m & 0x20) rect(x, y + k / 2, k, hh - k / 2, true);            // f
  if (m & 0x40) rect(x + k / 2, y + hh - k / 2, w - k, k, true);    // g
}
static const char *WD_FR[7] = { "dim", "lun", "mar", "mer", "jeu", "ven", "sam" };
static const char *WD_EN[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
#define WD (uiFr ? WD_FR : WD_EN)                  // v0.26.0: chosen when used
static float outsideC() { return wx.valid && nowS() - wx.at < 3 * 3600 ? wx.t : NAN; }
static String deg(float c) { return isnan(c) ? String("--") : String((int)lroundf(c)) + "\xC2\xB0"; }
static void drawClock();

static int batteryMv() {
  uint32_t sum = 0; for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(BAT_PIN);
  return sum / 8 * 2;
}
static float lipoPct(int mv) {             // this cell's curve, PROVISIONAL, from two drains logged 30 Sep - 8 Oct 2026:
                                            // 0% where the readings stop (3.50 V), 100% at rest after charging (4.03 V),
                                            // points between from the share of logged runtime still ahead (104 h in all).
                                            // v0.22.0 had 0% at 3.40 V: the cell never got there, both runs end at 3.50 - 3.52 V.
  static const int16_t V[] = { 3500, 3528, 3564, 3593, 3645, 3677, 3712, 3745, 3760, 3786, 3805,
                               3807, 3814, 3859, 3911, 3962, 3967, 3972, 3980, 3982, 4030 };
  if (mv <= V[0]) return 0; if (mv >= V[20]) return 100;
  int i = 0; while (mv > V[i + 1]) i++;
  return 5.0f * (i + (float)(mv - V[i]) / (V[i + 1] - V[i]));
}
// ---------------------------------------------------------------- battery watch: reserve, then the "battery empty" screen
// A reading is the one taken at the very start of a wake, before anything loads the cell (wakeMv). The reading of a
// cold start is not counted: it reads about 50 mV low. Thresholds from the two drains logged 30 Sep - 8 Oct 2026,
// PROVISIONAL: both runs end at 3.50 - 3.52 V. The watch runs on seconds since power-on, so it works with no clock set.
// While the device is awake (a long radio session, a long read) no reading is taken: the watch acts at the next wake.
#define BAT_RESERVE_MV 3550      // two readings in a row at or under this: reserve
#define BAT_EMPTY_MV   3510      // two readings in a row at or under this: the battery screen, then off
#define BAT_LEAVE_MV   3620      // one reading above this: out of reserve (it is charging)
#define BAT_DROP_MV    15        // in reserve, a fall of more than this between two 5-minute readings is NOTED in the field log.
                                 // It does not switch the device off: on 3 Oct 2026 the cell fell 24 mV between two readings
                                 // (3.538 to 3.514 V) and ran 4.5 hours more. The 5-minute readings will show what a real end looks like.
#define BAT_RESERVE_S  300       // in reserve, and after any low reading: a reading every 5 minutes
RTC_DATA_ATTR static uint8_t  rtcLowN, rtcEmptyN, rtcReserve;
RTC_DATA_ATTR static uint16_t rtcWatchMv;
RTC_DATA_ATTR static uint32_t rtcWatchAt;      // nowS() at the last counted reading; 0 = none since power-on
static bool reserveTurned = false;             // reserve began or ended at this wake: faces redraw once to show or drop the mark
static String mvText(int mv) { return String(mv / 1000.0f, 2) + " V"; }
static uint32_t batEvery() { return (rtcReserve || rtcLowN) ? BAT_RESERVE_S : LOG_EVERY_S; }
static bool batWatchDue() { return !rtcWatchAt || nowS() - rtcWatchAt >= batEvery() - 60; }
static bool batWatch(int mv) {             // one reading in; true = empty, show the battery screen and switch off
  if (!batWatchDue()) return false;
  int prev = rtcWatchMv; uint32_t prevAt = rtcWatchAt, now = nowS(); bool wasReserve = rtcReserve;
  rtcWatchAt = now ? now : 1;
  if (mv < 2800) { rtcLowN = rtcEmptyN = rtcReserve = 0; rtcWatchMv = 0; return false; }   // no cell: USB only
  rtcWatchMv = (uint16_t)mv;
  if (mv > BAT_LEAVE_MV) {
    if (rtcReserve) { reserveTurned = true; FL("battery", "out of reserve: " + mvText(mv)); }
    rtcLowN = rtcEmptyN = rtcReserve = 0; return false;
  }
  rtcLowN = mv <= BAT_RESERVE_MV ? (uint8_t)min(rtcLowN + 1, 250) : 0;
  rtcEmptyN = mv <= BAT_EMPTY_MV ? (uint8_t)min(rtcEmptyN + 1, 250) : 0;
  if (!rtcReserve && rtcLowN >= 2) {
    rtcReserve = 1; reserveTurned = true;
    nlSet(false, "battery reserve");           // v0.25.5
    FL("battery", "RESERVE: " + mvText(mv) + " on two readings. No background fetching, a reading every 5 min");
  }
  if (rtcEmptyN >= 2) { FL("battery", "EMPTY: " + mvText(mv) + " on two readings, switching off"); return true; }
  if (wasReserve && prev && prev - mv > BAT_DROP_MV && prevAt && now - prevAt <= 2 * BAT_RESERVE_S)
    FL("battery", "note: fell " + String(prev - mv) + " mV since the last reading (" + mvText(prev) + " to " + mvText(mv) + ")");
  return false;
}
static void batteryIcon() {                // top-right corner, only when a cell is present
  int mv = batteryMv();
  if (mv < 2800) return;
  int pct = (int)lipoPct(mv);
  rect(176, 3, 18, 1, true); rect(176, 11, 18, 1, true);
  rect(176, 3, 1, 9, true);  rect(193, 3, 1, 9, true); rect(194, 5, 2, 5, true);
  if (rtcReserve) { rect(184, 5, 2, 3, true); rect(184, 9, 2, 1, true); rect(176, 2, 18, 1, true); rect(176, 12, 18, 1, true); return; }   // reserve: an empty cell, drawn heavier, with a mark inside
  rect(178, 5, 14 * pct / 100, 5, true);
}
#if EK_LOG
static bool fsUp = false;
static void batFlush() {                    // RTC buffer -> flash file
  if (!rtcBatN) return;
  if (!fsUp) fsUp = LittleFS.begin(true, "/lfs", 5, "spiffs");
  if (!fsUp) return;
  if (!logMigrate()) return;                // never append new records to a log still in the old format
  File f = LittleFS.open("/log.bin", "a");
  if (!f) return;
  f.write((uint8_t *)rtcBat, rtcBatN * sizeof(BatSample)); f.close();
  rtcBatN = 0;
}

// ---------------------------------------------------------------- pressure sensor (BMP388 / BMP390 on the header)
// Probed once per boot at 0x76 and 0x77; silently absent when no module is fitted.
static i2c_master_dev_handle_t bmpDev = NULL; static bool bmpTried = false;
static uint8_t bmpAt = 0, bmpId = 0;                    // v0.25.3: its address and chip id (0x50 BMP388, 0x60 BMP390), for the Sensors page
// v0.25.4: the module's power. With its power wire on header pin GP2 (next to 3V3) the firmware switches it: on for a
// reading (about 50 ms every 30 minutes), on while the Sensors page is open, off otherwise and through sleep. The module's
// LED, regulator and level shifters drew all the time on 3V3: on 24-25.10 the nights drained 10 to 18 mV/h against 1 to 6
// without the sensor. Switched off, the module leaves the shared bus alone (tested by hand on 25.10: bus 18 38 51 70,
// touch working, no glow from the LED). On 3V3 it still works, always on: the probe tells which.
#define BMP_PWR_PIN 2
static bool bmpSwitched = false, bmpOn = false, bmpHold = false;   // bmpHold: the Sensors page keeps it on
// v0.25.5: the night light. Kept through sleep, not through a restart (no stored setting: ek_shared.h is not touched).
#define NL_IDLE_S (15 * 60)                    // off by itself this long after the last touch or button press
#define NL_Y      118                          // the Device tile: below this line, a tap is the lamp switch
RTC_DATA_ATTR static uint8_t  rtcNlOn;
RTC_DATA_ATTR static uint32_t rtcNlUntil;
RTC_DATA_ATTR static uint8_t  rtcBmpWire;      // where the sensor's power was found: 0 not found, 1 GP2, 2 3V3
// v0.25.6: three power cuts with a full cell on 25-26.10, each in a window holding a 30-minute reading, none under v0.25.3
// (purple on 3V3, nothing switched). So: (1) GP2 drives weakly (about 10 mA instead of about 20, Espressif's drive steps),
// which softens the rush into the module's capacitors and caps the current if the wire's socket touches a neighbouring pin;
// (2) 50 ms to settle instead of 20; (3) the wiring, once known, is not tested again by switching GP2 off and on at every
// wake; (4) a breadcrumb in the stored settings ("bmpStep") around each real switch, which a power cut cannot wipe:
// 1 switching on, 2 on, 3 switching off, 0 off. The next start logs it.
static void bmpStep(uint32_t st) { prefs.putUInt("bmpStep", st); }
// v0.26.2: "off" means released. Up to v0.26.1 the pin was driven low and held low through sleep, which pulled the
// module's supply to ground while its bus lines still saw 3.3 V from the pull-ups: the module was reverse-fed for hours
// at a time, and six runs on GP2 (20-25) ended in a power cut during sleep, none on 3V3. Released, the pin sources and
// sinks nothing: the module's rail floats to wherever the bus diodes leave it, and the firmware's only act is to drive it
// high for a reading. If the cuts stop, the active low was the trigger; if they go on, it is the wire or the module.
static void bmpRelease() { pinMode(BMP_PWR_PIN, INPUT); gpio_set_pull_mode((gpio_num_t)BMP_PWR_PIN, GPIO_FLOATING); }
static void bmpPower(bool on) {
  if (on && bmpOn) return;
  bool change = on != bmpOn;
  if (change) bmpStep(on ? 1 : 3);
  gpio_hold_dis((gpio_num_t)BMP_PWR_PIN);
  if (on) {
    pinMode(BMP_PWR_PIN, OUTPUT);
    gpio_set_drive_capability((gpio_num_t)BMP_PWR_PIN, GPIO_DRIVE_CAP_1);
    digitalWrite(BMP_PWR_PIN, HIGH);
    delay(50);                                           // the module's regulator, then the chip's own start (2 ms in Bosch's datasheet)
  } else bmpRelease();                                   // v0.26.2: off = released, never driven low
  bmpOn = on;
  if (change) bmpStep(on ? 2 : 0);
}
static void nlSet(bool on, const char *why) {          // the night light on or off; why: for the log, when it goes off
  if (on == (bool)rtcNlOn || (on && rtcBmpWire == 2)) return;   // on 3V3 the LED is always on: nothing to switch
  rtcNlOn = on; rtcNlUntil = on ? nowS() + NL_IDLE_S : 0;
  if (on) bmpPower(true); else if (!bmpHold) bmpPower(false);
  flEvent("light", on ? String("night light on") : String("night light off: ") + why);
}
static struct { float t1, t2, t3, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11; } bmpc;
static bool bmpInit() {
  if (bmpTried) return bmpDev != NULL;
  bmpTried = true;
  i2c_master_bus_handle_t bus = NULL;                      // the board's I2C bus, already opened by the Waveshare driver
  if (i2c_master_get_bus_handle(I2C_NUM_0, &bus) != ESP_OK || !bus) return false;
  const uint8_t addrs[2] = { 0x76, 0x77 };
  // v0.25.5: with the night light on, the wiring is already known (GP2): no test with GP2 off, so the light does not blink
  for (int pass = rtcBmpWire == 1 ? 1 : 0; pass < 2; pass++) {   // v0.25.6: wiring known (GP2): no test with GP2 off   // v0.25.4: first with GP2 off (a sensor that answers is on 3V3), then on
  bmpPower(pass == 1);
  for (int k = 0; k < 2; k++) {
    if (i2c_master_probe(bus, addrs[k], 50) != ESP_OK) continue;
    i2c_device_config_t cfg = {}; cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7; cfg.device_address = addrs[k]; cfg.scl_speed_hz = 300000;
    i2c_master_dev_handle_t h; if (i2c_master_bus_add_device(bus, &cfg, &h) != ESP_OK) continue;
    uint8_t id = 0, c[21];
    if (i2c_read_buff(h, 0x00, &id, 1) != 0 || (id != 0x50 && id != 0x60) || i2c_read_buff(h, 0x31, c, 21) != 0) { i2c_master_bus_rm_device(h); continue; }
    uint16_t T1 = c[0] | (c[1] << 8), T2 = c[2] | (c[3] << 8); int8_t T3 = c[4];
    int16_t P1 = c[5] | (c[6] << 8), P2 = c[7] | (c[8] << 8); int8_t P3 = c[9], P4 = c[10];
    uint16_t P5 = c[11] | (c[12] << 8), P6 = c[13] | (c[14] << 8); int8_t P7 = c[15], P8 = c[16];
    int16_t P9 = c[17] | (c[18] << 8); int8_t P10 = c[19], P11 = c[20];
    bmpc.t1 = T1 / 0.00390625f; bmpc.t2 = T2 / 1073741824.0f; bmpc.t3 = T3 / 281474976710656.0f;            // Bosch datasheet, floating point
    bmpc.p1 = (P1 - 16384) / 1048576.0f; bmpc.p2 = (P2 - 16384) / 536870912.0f; bmpc.p3 = P3 / 4294967296.0f;
    bmpc.p4 = P4 / 137438953472.0f; bmpc.p5 = P5 / 0.125f; bmpc.p6 = P6 / 64.0f; bmpc.p7 = P7 / 256.0f;
    bmpc.p8 = P8 / 32768.0f; bmpc.p9 = P9 / 281474976710656.0f; bmpc.p10 = P10 / 281474976710656.0f; bmpc.p11 = P11 / 36893488147419103232.0f;
    bmpDev = h; bmpAt = addrs[k]; bmpId = id; bmpSwitched = pass == 1; rtcBmpWire = pass == 1 ? 1 : 2; return true;   // switched: left on for the caller
  }
  }
  rtcBmpWire = 0; if (rtcNlOn) { rtcNlOn = 0; flEvent("light", "night light off: no sensor module answers"); }
  bmpPower(false); return false;
}
// n forced measurements, averaged. osr: the oversampling register (0x03: pressure x8, temperature x1; 0x0C: pressure x16,
// temperature x2), waitMs: long enough for one conversion at that setting (Bosch's formula: about 21 ms and 37 ms).
static bool bmpMeasure(uint8_t osr, int waitMs, int n, float &hPa, float &tC) {
  if (!bmpInit()) return false;
  double sp = 0, st = 0;
  for (int k = 0; k < n; k++) {
    uint8_t v = osr; if (i2c_write_buff(bmpDev, 0x1C, &v, 1) != 0) return false;
    v = 0x13; if (i2c_write_buff(bmpDev, 0x1B, &v, 1) != 0) return false;
    delay(waitMs);
    uint8_t d[6]; if (i2c_read_buff(bmpDev, 0x04, d, 6) != 0) return false;
    uint32_t up = d[0] | (d[1] << 8) | ((uint32_t)d[2] << 16), ut = d[3] | (d[4] << 8) | ((uint32_t)d[5] << 16);
    float pd1 = (float)ut - bmpc.t1, pd2 = pd1 * bmpc.t2, tc = pd2 + pd1 * pd1 * bmpc.t3;
    float t = tc, u = (float)up;
    float out1 = bmpc.p5 + bmpc.p6 * t + bmpc.p7 * t * t + bmpc.p8 * t * t * t;
    float out2 = u * (bmpc.p1 + bmpc.p2 * t + bmpc.p3 * t * t + bmpc.p4 * t * t * t);
    float out3 = u * u * (bmpc.p9 + bmpc.p10 * t) + u * u * u * bmpc.p11;
    sp += (out1 + out2 + out3) / 100.0; st += tc;
  }
  hPa = (float)(sp / n); tC = (float)(st / n);
  return hPa > 300 && hPa < 1250;
}
static bool bmpRead(float &hPa, float &tC) {   // the 30-minute sample: one forced measurement, pressure x8
  if (!bmpInit()) return false;
  if (bmpSwitched) bmpPower(true);
  bool ok = bmpMeasure(0x03, 25, 1, hPa, tC);
  if (bmpSwitched && !bmpHold && !rtcNlOn) bmpPower(false);   // v0.25.5: the night light keeps it on
  return ok;
}
static float toSeaLevel(float hPa, float tC) { float h = altM; return hPa * powf(1 - 0.0065f * h / (tC + 0.0065f * h + 273.15f), -5.257f); }
// v0.25.3: the sensor's own recent readings (sea level x10), one kept per 25 minutes or more, for the 3-hour trend.
// They survive deep sleep, not a power-off: after one, the trend waits 3 hours for the sensor's own readings.
#define PH_N 10
RTC_DATA_ATTR static uint32_t rtcPhT[PH_N];
RTC_DATA_ATTR static uint16_t rtcPhV[PH_N];
RTC_DATA_ATTR static uint32_t rtcPLocalAt;      // when the sensor last answered a sample
static void phPush(uint32_t t, uint16_t v) {
  uint32_t last = 0; for (int i = 0; i < PH_N; i++) if (rtcPhT[i] > last) last = rtcPhT[i];
  if (last && t < last) memset(rtcPhT, 0, sizeof rtcPhT);           // the clock went back: start over
  else if (last && t - last < 25 * 60) return;                      // reserve samples every 5 minutes: keep one in five
  int k = 0; for (int i = 1; i < PH_N; i++) if (rtcPhT[i] < rtcPhT[k]) k = i;   // an empty slot, or the oldest
  rtcPhT[k] = t; rtcPhV[k] = v;
}
static float phAgo(uint32_t now, uint32_t back) {   // the sensor's reading closest to `back` seconds ago, within 45 minutes; NAN if none
  int best = -1; uint32_t bd = 45 * 60 + 1;
  for (int i = 0; i < PH_N; i++) {
    if (!rtcPhT[i] || rtcPhT[i] > now) continue;
    uint32_t age = now - rtcPhT[i], d = age > back ? age - back : back - age;
    if (d < bd) { bd = d; best = i; }
  }
  return best < 0 ? NAN : rtcPhV[best] / 10.0f;
}
// The sensor's value counts while it is under 75 minutes old. Before, the last value stayed on screen after the sensor was unplugged.
static bool pLocalFresh() { return wx.pLocal > 0 && (uint32_t)time(NULL) - rtcPLocalAt < 75 * 60; }

static bool logCanSample() { return wakeMv >= 2800 && timeValid(); }
static bool coldStart = false;               // this start is not a wake: its battery reading reads low
static void batSample() {                   // at most one sample per 30 min (per 5 min in reserve)
  if (!logCanSample()) return;
  uint32_t now = time(NULL);
  if (rtcBatAt && now - rtcBatAt < batEvery() - 60) return;
  float out = outsideC();
  float hp, tc; uint16_t p10 = 0;
  bool pOk = bmpRead(hp, tc); flSensor(bmpDev != NULL);
  if (pOk) { float msl = toSeaLevel(hp, tc); p10 = (uint16_t)lroundf(msl * 10); wx.pLocal = msl; rtcPLocalAt = now; phPush(now, p10); }   // v0.25.3: pLocal3h is no longer used
  if (rtcBatN >= 16) { memmove(rtcBat, rtcBat + 1, 15 * sizeof(BatSample)); rtcBatN = 15; }   // flash kept refusing: drop the oldest, never write past the buffer
  rtcBat[rtcBatN++] = { now, (uint16_t)wakeMv, rtcWifiN, rtcAskN,
    (int16_t)(isnan(insideC) ? INT16_MIN : lroundf(insideC * 10)),
    (uint8_t)(isnan(insideRH) ? 255 : constrain(lroundf(insideRH), 0, 100)),
    (int8_t)(isnan(out) ? -128 : constrain(lroundf(out), -60, 60)), p10,
    (uint16_t)((coldStart ? LOGF_COLD : 0) | (rtcReserve ? LOGF_RESERVE : 0)) };
  rtcBatAt = now; rtcWifiN = 0; rtcAskN = 0;
  if (rtcBatN >= 4 || rtcReserve) batFlush();    // v0.26.1: every 2 h (was 6); in reserve at once, the end may come without warning
}
// A v1 file (12-byte records) becomes v2 (16 bytes), once. True when the log is in the v2 format.
// The format is remembered in the settings ("logV": 0 = not settled yet, 1 = swap under way, 2 = v2).
// v0.23.0 guessed the format from the file size: a v1 log whose record count was a multiple of four (the sampler writes
// twelve at a time) has a size that also divides by 16, was taken for v2, and v2 records were then appended to it.
// v0.23.1 (never flashed) trusted the setting alone and would have converted a file v0.23.0 had already converted.
// v0.23.2 settles it by READING the records: a time stamp between 2025 and 2035 and a cell voltage are hard to fake.
//   every record reads as v2            -> nothing to do
//   every record reads as v1            -> all converted
//   v1 at the front, v2 behind it       -> the front converted, the rest kept (what v0.23.0 left on some devices)
//   none of these                       -> the file is set aside as /log.bad, untouched, and a new log starts
// The new file is written beside the old one and swapped in, so a power cut in the middle cannot lose the log.
static bool logRecOk(const uint8_t *p, bool v2) {
  BatSampleV1 a; memcpy(&a, p, sizeof a);
  if (a.t < 1735689600UL || a.t > 2051222400UL) return false;          // 2025-01-01 .. 2035-01-01
  if (a.mv < 2800 || a.mv > 5000) return false;
  if (a.rh > 100 && a.rh != 255) return false;
  if (a.tin10 != INT16_MIN && (a.tin10 < -400 || a.tin10 > 800)) return false;
  if (a.tout != -128 && (a.tout < -60 || a.tout > 60)) return false;
  if (!v2) return true;
  uint16_t p10, fl; memcpy(&p10, p + 12, 2); memcpy(&fl, p + 14, 2);
  return (p10 == 0 || (p10 >= 3000 && p10 <= 11000)) && fl <= 3;
}
static bool logMigrate() {
  static bool ok = false; if (ok) return true;
  uint8_t v = prefs.getUChar("logV", 0);
  if (v >= 2) return ok = true;
  if (v == 1) {                                // the swap was interrupted: finish it
    if (LittleFS.exists("/log.v2")) { LittleFS.remove("/log.bin"); LittleFS.rename("/log.v2", "/log.bin"); }
    prefs.putUChar("logV", 2); FL("log", "30-minute log: an interrupted migration was finished");
    return ok = true;
  }
  if (LittleFS.exists("/log.v2")) LittleFS.remove("/log.v2");   // a half-written copy from an earlier try
  if (!LittleFS.exists("/log.bin")) { prefs.putUChar("logV", 2); return ok = true; }   // no log yet: it starts as v2
  File f = LittleFS.open("/log.bin", "r"); if (!f) return false;
  size_t sz = f.size();
  if (sz < 12) { f.close(); LittleFS.remove("/log.bin"); prefs.putUChar("logV", 2); return ok = true; }
  uint8_t *d = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  if (!d) { f.close(); FL("log", "30-minute log: MIGRATION WAITING, no memory for " + String((unsigned long)sz) + " bytes"); return false; }
  size_t got = f.read(d, sz); f.close();
  if (got != sz) { free(d); FL("log", "30-minute log: MIGRATION WAITING, the old file could not be read"); return false; }
  // ---- what is it?
  size_t n2 = sz / 16, lead2 = 0, n1 = sz / 12, lead1 = 0;
  while (lead2 < n2 && logRecOk(d + lead2 * 16, true)) lead2++;
  while (lead1 < n1 && logRecOk(d + lead1 * 12, false)) lead1++;
  if (n2 && lead2 == n2 && sz % 16 == 0) {     // v2 already (v0.23.0 converted it, or it was born v2)
    free(d); prefs.putUChar("logV", 2);
    FL("log", "30-minute log: " + String((unsigned long)n2) + " records, already in the 16-byte format, left as they are");
    return ok = true;
  }
  size_t k = 0, rest = 0;                      // k v1 records at the front, then rest v2 records
  if (lead1 == n1 && sz % 12 == 0) k = n1;     // v1 all the way
  else {
    for (size_t c = lead1; c >= 1; c--) {      // v1 at the front, v2 behind: find where one ends and the other begins
      size_t tail = sz - c * 12; if (!tail || tail % 16) continue;
      size_t m = tail / 16, g = 0; while (g < m && logRecOk(d + c * 12 + g * 16, true)) g++;
      if (g == m) { k = c; rest = m; break; }
    }
    if (!k && lead1 == n1) k = n1;             // v1 all the way, with a few stray bytes at the end: they are dropped
  }
  if (!k && n2 && lead2 == n2) {               // v2 with a few stray bytes at the end: keep the records, drop the tail
    File g = LittleFS.open("/log.v2", "w"); size_t out = g ? g.write(d, n2 * 16) : 0; if (g) g.close();
    free(d);
    if (out != n2 * 16) { LittleFS.remove("/log.v2"); FL("log", "30-minute log: MIGRATION WAITING, flash refused the new file"); return false; }
    prefs.putUChar("logV", 1); LittleFS.remove("/log.bin"); LittleFS.rename("/log.v2", "/log.bin"); prefs.putUChar("logV", 2);
    FL("log", "30-minute log: " + String((unsigned long)n2) + " records in the 16-byte format kept, " + String((int)(sz % 16)) + " stray bytes dropped");
    return ok = true;
  }
  if (!k) {                                    // not a log we can read: set it aside, untouched, and start a new one
    free(d);
    if (LittleFS.exists("/log.bad")) LittleFS.remove("/log.bad");
    if (!LittleFS.rename("/log.bin", "/log.bad")) { FL("log", "30-minute log: MIGRATION WAITING, the unreadable file could not be set aside"); return false; }
    prefs.putUChar("logV", 2);
    FL("log", "30-minute log: UNREADABLE (" + String((unsigned long)sz) + " bytes, neither format). Set aside as /log.bad, a new log starts");
    return ok = true;
  }
  File g = LittleFS.open("/log.v2", "w"); size_t out = 0;
  if (g) {
    for (size_t i = 0; i < k; i++) { BatSampleV1 a; memcpy(&a, d + i * 12, sizeof a); BatSample s2 = { a.t, a.mv, a.wifi, a.asks, a.tin10, a.rh, a.tout, 0, 0 }; out += g.write((uint8_t *)&s2, sizeof s2); }
    if (rest) out += g.write(d + k * 12, rest * 16);
    g.close();
  }
  BatSampleV1 a0, a1; memcpy(&a0, d, sizeof a0); memcpy(&a1, d + (k - 1) * 12, sizeof a1);
  size_t stray = rest ? 0 : sz - k * 12;
  free(d);
  if (out != (k + rest) * sizeof(BatSample)) { LittleFS.remove("/log.v2"); FL("log", "30-minute log: MIGRATION WAITING, flash refused the new file"); return false; }
  prefs.putUChar("logV", 1);
  LittleFS.remove("/log.bin"); LittleFS.rename("/log.v2", "/log.bin");
  prefs.putUChar("logV", 2);
  String note = "30-minute log: " + String((unsigned long)k) + " records migrated to the 16-byte format, first T" + String(a0.t) + ", last T" + String(a1.t);
  if (rest) note += ", " + String((unsigned long)rest) + " newer records were already in it and were kept";
  if (stray) note += ", " + String((int)stray) + " stray bytes dropped";
  FL("log", note);
  return ok = true;
}
// A sample needs its own time stamp in an export when it does not follow the one before at the 30-minute rhythm:
// the first one, after a pause, after the clock went backwards, and (v0.23.2) when it comes early, as the 5-minute
// reserve readings and cold-start readings do. Before, an early sample was dated 30 minutes after its neighbour.
static bool logGap(const BatSample *a, int i, int first) {
  if (i <= first) return true;
  if (a[i].t < a[i - 1].t) return true;
  uint32_t dt = a[i].t - a[i - 1].t;
  return dt > LOG_EVERY_S + 600 || dt + 600 < LOG_EVERY_S;
}
// One record as a line of the measurements table (the logs page). Empty cell = no value.
#define LOG_CSV_HEAD "time_utc,epoch,battery_mv,wifi,asks,room_c,room_rh,out_c,pressure_hpa,flags\n"
static int logCsvLine(const BatSample &s, char *b, size_t cap) {
  time_t e = s.t; struct tm t; gmtime_r(&e, &t);
  char room[12] = "", rh[8] = "", out[8] = "", pr[12] = "";
  if (s.tin10 != INT16_MIN) snprintf(room, sizeof room, "%s%d.%d", s.tin10 < 0 ? "-" : "", abs(s.tin10) / 10, abs(s.tin10) % 10);
  if (s.rh != 255) snprintf(rh, sizeof rh, "%u", (unsigned)s.rh);
  if (s.tout != -128) snprintf(out, sizeof out, "%d", (int)s.tout);
  if (s.p10) snprintf(pr, sizeof pr, "%u.%u", (unsigned)(s.p10 / 10), (unsigned)(s.p10 % 10));
  return snprintf(b, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ,%lu,%u,%u,%u,%s,%s,%s,%s,%u\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec, (unsigned long)s.t, (unsigned)s.mv, (unsigned)s.wifi, (unsigned)s.asks, room, rh, out, pr, (unsigned)s.spare);
}
static BatSample *loadLog(int &n) {          // whole log into PSRAM (caller frees)
  batFlush();
  n = 0; BatSample *a = NULL;
  if (fsUp || (fsUp = LittleFS.begin(true, "/lfs", 5, "spiffs"))) {
    File f; if (logMigrate()) f = LittleFS.open("/log.bin", "r");
    if (f) {
      n = f.size() / sizeof(BatSample);
      a = (BatSample *)heap_caps_malloc(max(n, 1) * sizeof(BatSample), MALLOC_CAP_SPIRAM);
      if (a) f.read((uint8_t *)a, n * sizeof(BatSample)); else n = 0;
      f.close();
    }
  }
  return a;
}
#endif
#if BATLOG
static void drawBattery() {
  int n; BatSample *a = loadLog(n);
  int mv = batteryMv();
  textAt(EK_MID, 100, 18, String(mv / 1000.0f, 2) + " V  " + String((int)lipoPct(mv)) + "%");
  // since the last charge: the last point where voltage jumped up
  int s0 = 0;
  for (int i = 1; i < n; i++) if (a[i].mv > a[i - 1].mv + 25) s0 = i;   // still charging up to here
  // graph: the last 7 days, 3.3 .. 4.25 V
  const int GX = 8, GY = 46, GW = 184, GH = 80;
  rect(GX, GY + GH, GW, 1, true); rect(GX, GY, 1, GH, true);
  for (int d = 1; d < 7; d++) rect(GX + GW * d / 7, GY + GH - 3, 1, 3, true);   // day ticks
  uint32_t now = time(NULL), from = now - 7UL * 86400;
  for (int i = 0; i < n; i++) {
    if (a[i].t < from) continue;
    int x = GX + (int)((uint64_t)(a[i].t - from) * (GW - 1) / (7UL * 86400));
    int y = GY + GH - 1 - constrain((a[i].mv - 3300) * (GH - 1) / 950, 0, GH - 1);
    rect(x, y - 1, 2, 3, true);
  }
  // rate: straight-line fit of % against time since the last charge
  String l1 = "measuring...", l2 = String(n) + " samples", l3 = "";
  if (n - s0 >= 2) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = n - s0; uint32_t t0 = a[s0].t;
    long wifi = 0, asks = 0;
    for (int i = s0; i < n; i++) {
      double x = (a[i].t - t0) / 86400.0, y = lipoPct(a[i].mv);
      sx += x; sy += y; sxx += x * x; sxy += x * y; wifi += a[i].wifi; asks += a[i].asks;
    }
    double span = (a[n - 1].t - t0) / 86400.0;
    if (span >= 0.25) {
      double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);          // %/day, negative
      if (slope < -0.05) {
        l1 = String(-slope, 1) + " %/day (" + String(span, 1) + " d)";
        double left = lipoPct(mv) / -slope;                              // days
        l2 = left < 1 ? "~" + String((int)(left * 24)) + " hours left" : "~" + String((int)left) + " days left";
      } else l1 = "barely moving";
      l3 = String((int)(wifi / span)) + " wifi  " + String((int)(asks / span)) + " asks /day";
    } else l2 = "needs 6 h of data";
  }
  textAt(EK_SMALL, 100, 132, l1); textAt(EK_SMALL, 100, 150, l2);
  if (l3.length()) textAt(EK_SMALL, 100, 168, l3);
  if (a) free(a);
}
#else
static void drawBattery() {}
#endif
#if CLIMATE
// one series on a graph box: 7 days wide, scaled to lo..hi; dotted = every 3rd sample only
static void series(BatSample *a, int n, uint32_t from, int gx, int gy, int gw, int gh,
                   float lo, float hi, int which, bool dotted) {
  int k = 0;
  for (int i = 0; i < n; i++) {
    if (a[i].t < from) continue;
    float v = which == 0 ? (a[i].tin10 == INT16_MIN ? NAN : a[i].tin10 / 10.0f)
            : which == 1 ? (a[i].tout == -128 ? NAN : a[i].tout)
            : (a[i].rh == 255 ? NAN : a[i].rh);
    if (isnan(v)) continue;
    if (dotted && (k++ % 3)) continue;
    int x = gx + (int)((uint64_t)(a[i].t - from) * (gw - 1) / (7UL * 86400));
    int y = gy + gh - 1 - constrain((int)((v - lo) / (hi - lo) * (gh - 1)), 0, gh - 1);
    if (dotted) rect(x, y, 2, 2, true); else rect(x, y - 1, 2, 3, true);
  }
}
static void drawClimate() {
  int n; BatSample *a = loadLog(n);
  String now = L("in ", "int. ") + deg(insideC) + "  " + (isnan(insideRH) ? String("--") : String((int)lroundf(insideRH))) + "%";
  textAt(EK_MID, 100, 18, now);
  uint32_t from = time(NULL) - 7UL * 86400;
  float tlo = 99, thi = -99, hlo = 100, hhi = 0; int used = 0;
  for (int i = 0; i < n; i++) {
    if (a[i].t < from) continue;
    if (a[i].tin10 != INT16_MIN) { float v = a[i].tin10 / 10.0f; tlo = fminf(tlo, v); thi = fmaxf(thi, v); used++; }
    if (a[i].tout != -128) { tlo = fminf(tlo, a[i].tout); thi = fmaxf(thi, a[i].tout); }
    if (a[i].rh != 255) { hlo = fminf(hlo, a[i].rh); hhi = fmaxf(hhi, a[i].rh); }
  }
  if (!used) { textBlock(L("the room graph\nfills in over\nthe next hours", "le graphique\nse remplit dans\nles heures \xC3\xA0" " venir"), 8, 60, 184, 110, false); if (a) free(a); return; }
  tlo = floorf(tlo) - 1; thi = ceilf(thi) + 1; if (thi - tlo < 6) { float m = (thi + tlo) / 2; tlo = m - 3; thi = m + 3; }
  hlo = floorf(hlo / 5) * 5 - 5; hhi = ceilf(hhi / 5) * 5 + 5; if (hhi - hlo < 20) { float m = (hhi + hlo) / 2; hlo = m - 10; hhi = m + 10; }
  hlo = fmaxf(hlo, 0); hhi = fminf(hhi, 100);
  // temperature box
  const int GX = 36, GW = 156, TY = 46, TH = 70, HY = 128, HH = 40;
  rect(GX, TY + TH, GW, 1, true); rect(GX, TY, 1, TH, true);
  rect(GX, HY + HH, GW, 1, true); rect(GX, HY, 1, HH, true);
  for (int d = 1; d < 7; d++) { rect(GX + GW * d / 7, TY + TH - 3, 1, 3, true); rect(GX + GW * d / 7, HY + HH - 3, 1, 3, true); }
  textAt(EK_SMALL, 17, TY - 2, String((int)thi) + "\xC2\xB0");
  textAt(EK_SMALL, 17, TY + TH - 16, String((int)tlo) + "\xC2\xB0");
  textAt(EK_SMALL, 17, HY - 2, String((int)hhi) + "%");
  textAt(EK_SMALL, 17, HY + HH - 16, String((int)hlo) + "%");
  series(a, n, from, GX + 2, TY, GW - 2, TH, tlo, thi, 1, true);      // outdoors: dotted
  series(a, n, from, GX + 2, TY, GW - 2, TH, tlo, thi, 0, false);     // room: solid
  series(a, n, from, GX + 2, HY, GW - 2, HH, hlo, hhi, 2, false);     // room humidity
  textAt(EK_SMALL, 100, 176, L("7 days: solid in, dots out", "trait int. \xC2\xB7" " points ext."));
  if (a) free(a);
}
#else
static void drawClimate() {}
#endif
#if !EK_LOG
static void batFlush() {}
static void batSample() {}
#endif

static void tileDots() {                   // which tile am I on: filled dot
  // v0.25.0: with the device tile Classic has 12 tiles, and centred dots ran into the battery sign (x 176).
  // The row stays left of x 170: centred while it fits, then pushed left, then closer together (13 tiles at most).
  int n = N_TILES, sp = n > 1 ? min(14, 156 / (n - 1)) : 14, cur = tilePos(tile);
  int x0 = min(100 - (n - 1) * sp / 2, 166 - (n - 1) * sp);
  for (int i = 0; i < n; i++) {
    if (i == cur) disc(x0 + i * sp, 8, 4, true);
    else ring(x0 + i * sp, 8, 4, 1, false);
  }
  batteryIcon();
}
static void bookIcon(int cx, int cy);
static void catalogFace();
static void radioDraw(int st);
static void radioRun();
static void climFace();
static void deviceFace();
static void ksFace();
// a small crossed-out Wi-Fi mark (15 x 12): "this is the last known data, we're offline"
static void wifiOffIcon(int x, int y) {
  for (int r = 3; r <= 11; r += 4)
    for (int a = -45; a <= 45; a += 3) {
      float t = a * PI / 180; px(x + 7 + (int)(sinf(t) * r), y + 12 - (int)(cosf(t) * r), true);
    }
  rect(x + 6, y + 11, 3, 2, true);
  for (int k = 0; k < 15; k++) { px(x + k, y + k * 13 / 15, true); px(x + k + 1, y + k * 13 / 15, true); }
}
#if EK_INVISIBLEOCEAN
static bool boxMode = false;                 // fresh out of the box: show the welcome screen
static void drawBox();
#endif
static void screenIdle(const char *line = NULL) {
  epd->EPD_Clear();
#if EK_INVISIBLEOCEAN
  if (boxMode) { drawBox(); show(); onIdle = true; return; }
#endif
  tileDots();
  if (tile == T_CLOCK) { drawClock(); show(); onIdle = true; return; }
  if (tile == T_CATALOG) { catalogFace(); show(); onIdle = true; return; }
  if (tile == T_RADIO) { radioDraw(0); return; }
  if (tile == T_KAFFEE) { ksFace(); return; }
  if (tile == T_EDITIONS) {
    bookIcon(100, 66);
    textAt(EK_MID, 100, 100, L("editions", "\xC3\xA9" "ditions"));
    String left = edLeftOut();                 // v0.25.2: packs the fonts cannot draw are named here
    if (left.length()) textAt(EK_SMALL, 100, 132, L("left out: ", "\xC3\xA9" "cart\xC3\xA9" "\xC2\xA0" ": ") + left);
    else textAt(EK_SMALL, 100, 132, L("offline card packs", "fiches hors ligne"));
    textAt(EK_SMALL, 100, 164, L("tap to open", "touchez pour ouvrir"));
    show(); onIdle = true; return;
  }
  if (tile == T_BATTERY) { drawBattery(); show(); onIdle = true; return; }
  if (tile == T_DEVICE) { deviceFace(); show(); onIdle = true; return; }
#if CLICKER
  if (tile == T_CLICKER) { ckFace(); show(); onIdle = true; return; }
#endif
  if ((tile == T_ASK || tile == T_DERDIEDAS || tile == T_ORACLE || tile == T_JAR || tile == T_KAFFEE) && !wifiConfigured()) {
    wifiOffIcon(93, 52);                     // v0.25.0: the tile says what is missing, and where to fix it
    textAt(EK_MID, 100, 88, L("no Wi-Fi", "pas de Wi-Fi"));
    textAt(EK_SMALL, 100, 130, L("set it up on", "Ã " " configurer sur"));
    textAt(EK_SMALL, 100, 148, L("the device tile", "la tuile appareil"));
    show(); onIdle = true; return;
  }
  if (tile == T_CLIMATE) { climFace(); show(); onIdle = true; return; }
  // Weather, Oracle and Jar: arriving on the tile never waits for the network. The face shows what
  // was fetched before, or a splash; a middle tap fetches (like the radio: tap to play).
  if (tile == T_ORACLE) {
    if (jsonField(orBody, "answer").length()) drawOracle(orBody);
    else { sparkle(100, 60); textAt(EK_MID, 100, 100, "the oracle"); textAt(EK_SMALL, 100, 150, "tap to consult"); }
    show(); onIdle = true; return;
  }
  if (tile == T_JAR) {
    if (jsonField(jarBody, "answer").length() && !jarAnswered) drawJar(jarBody);
    else { jarIcon(100, 62); textAt(EK_MID, 100, 100, "the jar"); textAt(EK_SMALL, 100, 142, jarAnswered ? "tap for another" : "tap for a question"); textAt(EK_SMALL, 100, 164, "hold to answer"); }
    show(); onIdle = true; return;
  }
  if (tile == T_DERDIEDAS) textBlock("der\ndie\ndas", 8, 18, 184, 116, true);
  else micIcon(100, 78);
  textBlock(line ? line : tileHint(tile), 8, 138, 184, 56, false);
  show();
  onIdle = true;
}
static void screenText(const String &s, bool big) {
  epd->EPD_Clear();
  textBlock(s, 8, 8, 184, 184, big);
  show();
  onIdle = false;
}
// der die das: article + noun large on top, plural and dative below a rule
static void screenGrammar(const String &s) {
  int nl = s.indexOf('\n');
  if (nl < 0) { screenText(s, true); return; }
  epd->EPD_Clear();
  textBlock(s.substring(0, nl), 8, 10, 184, 92, true);
  rect(30, 106, 140, 2, true);
  String rest = s.substring(nl + 1); rest.trim();
  textBlock(rest, 8, 114, 184, 80, false);
  show();
  onIdle = false;
}

// ---------------------------------------------------------------- touch
static bool touched() {
  uint8_t n = 0;
  if (i2c_read_buff(touch_handle, 0x02, &n, 1) != 0) return false;
  n &= 0x0F; return n == 1 || n == 2;         // anything else is a misread, not a finger
}
// first finger position; false if no finger. Works whichever way the panel is mounted.
static void waitRelease(uint32_t maxMs = 8000) {   // never wait forever for a finger to lift
  uint32_t t0 = millis(); while (touched() && millis() - t0 < maxMs) delay(12);
}
static bool touchXY(int &x, int &y) {
  uint8_t r[5];
  if (i2c_read_buff(touch_handle, 0x02, r, 5) != 0) return false;
  int n = r[0] & 0x0F; if (n != 1 && n != 2) return false;
  x = ((r[1] & 0x0F) << 8) | r[2];
  y = ((r[3] & 0x0F) << 8) | r[4];
  return true;
}
static void touchReset() {
  pinMode(TOUCH_RST_PIN, OUTPUT);
  digitalWrite(TOUCH_RST_PIN, HIGH); delay(50);
  digitalWrite(TOUCH_RST_PIN, LOW);  delay(50);
  digitalWrite(TOUCH_RST_PIN, HIGH); delay(150);
}

// ---------------------------------------------------------------- network
// Wi-Fi: the secrets may hold several networks, "Home|Hotspot" with "pass1|pass2"
#define MAX_NETS EK_NETS_MAX
RTC_DATA_ATTR static int8_t rtcNet = -1;      // index of the network that worked last
static EkNets wnets;                          // v0.25.0: the networks live in the settings, in every edition
static int nets = 0, tryNet = 0;
static bool netsLoaded = false;
static bool wifiStarted = false;
#define netName wnets.name
#define netPass wnets.pass
static void splitNets() {
  if (netsLoaded) return;
  netsLoaded = true;
  EkPrefsKV kv(prefs);
  wnets.load(kv);                             // a school unit's one network moves into the list here, once
#if !EK_INVISIBLEOCEAN
  if (!wnets.count && !prefs.getBool("wSeeded", false)) {   // Classic, first start of v0.25.0: the built-in list moves into the settings
    String a = EK_WIFI_SSID, b = EK_WIFI_PASS;
    while (a.length()) {
      int i = a.indexOf('|'), j = b.indexOf('|');
      wnets.add(kv, (i < 0 ? a : a.substring(0, i)).c_str(), (j < 0 ? b : b.substring(0, j)).c_str());
      a = i < 0 ? "" : a.substring(i + 1);
      b = j < 0 ? "" : b.substring(j + 1);
    }
    prefs.putBool("wSeeded", true);           // once: a network the owner removes does not come back
  }
#endif
  nets = wnets.count;
}
static void netsChanged() { netsLoaded = false; nets = 0; rtcNet = -1; wifiStarted = false; splitNets(); }
static volatile int wifiReason = 0;         // last disconnect reason for the current attempt
static int netReason[MAX_NETS];             // per network: why it failed (0 = not tried)
static bool evHooked = false;
static void wifiBegin(int n) {
  if (!evHooked) {
    evHooked = true;
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
      int r = info.wifi_sta_disconnected.reason;
      if (r != 8) wifiReason = r;           // 8 = we left on purpose: not a failure
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  }
  WiFi.disconnect(); WiFi.mode(WIFI_STA);
  delay(100);                               // let the old attempt's events drain
  wifiReason = 0;
  WiFi.setAutoReconnect(false);             // we decide when to move to the next network
  WiFi.begin(netName[n], netPass[n]);
  tryNet = n; wifiStarted = true;
}
static const char *reasonText(int r) {
  switch (r) {
    case 0:   return L("no answer", "pas de r\xC3\xA9" "ponse");
    case 201: return L("not visible", "introuvable");
    case 2: case 15: case 202: case 204: return L("wrong password?", "mot de passe erron\xC3\xA9" "\xC2\xA0" "?");
    case 210: case 211: return L("security mode (WPA3?)", "mode de s\xC3\xA9" "curit\xC3\xA9" " (WPA3\xC2\xA0" "?)");
    case 212: return L("signal too weak", "signal trop faible");
    case 203: return L("refused", "refus\xC3\xA9");
    default:  return L("failed", "\xC3\xA9" "chec");
  }
}
static String wifiDiag() {                  // one line per known network, for the paper
  int found = WiFi.scanNetworks(false, true);
  String d;
  for (int n = 0; n < nets; n++) {
    bool seen = false;
    for (int i = 0; i < found; i++) if (WiFi.SSID(i) == netName[n]) seen = true;
    String why = !seen ? L("not visible (2.4 GHz on?)", "introuvable (2,4\xC2\xA0" "GHz actif\xC2\xA0" "?)") : reasonText(netReason[n]);
    d += "\n" + String(netName[n]) + L(": ", "\xC2\xA0" ": ") + why;
  }
  WiFi.scanDelete();
  return d;
}
static String jsonSafe(String s) { s.replace("\\", "/"); s.replace("\"", "'"); s.replace("\n", "\\n"); return s; }
static void wifiStart() {                   // begin connecting in the background
  if (wifiStarted) return;
  splitNets();
  if (!nets) return;
  wk(3);                                    // v0.26.1
  if (rtcNet < 0 || rtcNet >= nets) rtcNet = prefs.getInt("net", 0);   // survives power-off too
  if (rtcNet < 0 || rtcNet >= nets) rtcNet = 0;
  wifiBegin(rtcNet);
}
static bool wifiUp() {
  if (WiFi.status() == WL_CONNECTED) return true;
  splitNets(); if (!nets) return false;       // nothing set up: fail at once
  if (!wifiStarted) wifiStart();
  else if (wifiReason) wifiBegin(tryNet);   // the earlier attempt is dead: start it again
  for (int k = 0; k < nets; k++) {          // last good network first, then the others
    uint32_t t0 = millis();
    while (millis() - t0 < 10000 && WiFi.status() != WL_CONNECTED &&
           !(wifiReason && millis() - t0 > 500)) delay(100);   // a real reason ends the attempt
    if (WiFi.status() == WL_CONNECTED) {
      FL("wifi", "up, network " + String(tryNet + 1) + " of " + String(nets) + ", waited " + flSecs(millis() - t0) + ", signal " + String(WiFi.RSSI()) + " dBm");
      if (!wifiCounted) { wifiCounted = true; if (rtcWifiN < 255) rtcWifiN++; }
      if (rtcNet != tryNet) { rtcNet = tryNet; prefs.putInt("net", tryNet); }
      return true;
    }
    netReason[tryNet] = wifiReason;
    FL("wifi", "FAILED, network " + String(tryNet + 1) + " of " + String(nets) + " after " + flSecs(millis() - t0) + ": " + reasonText(netReason[tryNet]) + " (reason " + String(netReason[tryNet]) + ")");
    if (k + 1 < nets) wifiBegin((tryNet + 1) % nets);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void writeWavHeader(uint8_t *h, uint32_t dataLen) {
  auto u32 = [&](int o, uint32_t v) { h[o] = v; h[o+1] = v >> 8; h[o+2] = v >> 16; h[o+3] = v >> 24; };
  auto u16 = [&](int o, uint16_t v) { h[o] = v; h[o+1] = v >> 8; };
  memcpy(h, "RIFF", 4); u32(4, 36 + dataLen); memcpy(h + 8, "WAVEfmt ", 8);
  u32(16, 16); u16(20, 1); u16(22, 1); u32(24, EK_SAMPLE_RATE);
  u32(28, EK_SAMPLE_RATE * 2); u16(32, 2); u16(34, 16);
  memcpy(h + 36, "data", 4); u32(40, dataLen);
}

// pull a JSON string field out of the relay's reply, unescaping it
static String jsonField(const String &body, const char *key) {
  String k = String("\"") + key + "\":\"";
  int i = body.indexOf(k); if (i < 0) return "";
  i += k.length(); String out;
  while (i < (int)body.length() && body[i] != '"') {
    char c = body[i++];
    if (c == '\\' && i < (int)body.length()) {
      char e = body[i++];
      if (e == 'n') out += '\n'; else if (e == 't') out += ' ';
      else if (e == 'u' && i + 4 <= (int)body.length()) {
        uint16_t cp = strtol(body.substring(i, i + 4).c_str(), NULL, 16); i += 4;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
      } else out += e;
    } else out += c;
  }
  return out;
}

// POST to the brain; returns the raw JSON body, or {"error":"..."} on failure
// force = the user asked (a voice question): always try. Otherwise respect the offline window,
// so tiles and timers answer at once instead of waiting on a network that isn't there.
static String callBrain(const char *mode, const char *ctype, const uint8_t *data, size_t len) {
  return callBrainF(mode, ctype, data, len, false);
}
static String callBrainF(const char *mode, const char *ctype, const uint8_t *data, size_t len, bool force) {
  if (!force && offlineNow()) return "{\"error\":\"offline\"}";
  if (!wifiUp()) {
    rtcNetDownUntil = nowS() + NET_BACKOFF_S;
    return force ? "{\"error\":\"no Wi-Fi" + jsonSafe(wifiDiag()) + "\"}" : "{\"error\":\"offline\"}";
  }
  rtcNetDownUntil = 0;
  maybeSyncTime();
  NetworkClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  if (!http.begin(tls, String(EK_BRAIN_URL) + "/api/ask")) return "{\"error\":\"cannot reach the brain\"}";
  http.addHeader("Content-Type", ctype);
  http.addHeader("x-device", "e154");
  http.addHeader("x-mode", mode);
  http.addHeader("x-key", EK_RELAY_KEY);
  uint32_t flT = millis();
  int code = http.POST((uint8_t *)data, len);
  String body = code > 0 ? http.getString() : "";
  FL("brain", String(mode) + ": http " + String(code) + ", " + String(body.length()) + " bytes in " + flSecs(millis() - flT));
  http.end();
  if (code <= 0) return "{\"error\":\"network error\"}";
  return body;
}


// ================================================================ EDITIONS: offline card packs
// Packs: Edition Zero is built in; more live on microSD in /editions/<slug>/ (see EDITIONS.md).
// In the reader: left third = back, right third = next, middle = act (reveal, tick),
// hold = up one level, PWR = back to the tiles. No microphone and no Wi-Fi in the reader.
#define RD_AWAKE_MS   60000                 // reading: stay awake a bit longer between touches
#define RD_HOLD_MS    600                   // hold this long (still) to go up one level
#define TOUCH_FLIP_X  0                     // set to 1 if the left/right thirds come out swapped
#define ED_MAX        12
static bool smallText = false;              // reader setting: body text large (MID) or small

// ---- a tiny JSON reader (whole document in PSRAM, nodes by index)
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
struct JNode { uint8_t t; bool b; float n; const char *s; const char *key; int child, next, count; };
static JNode *jdoc = NULL; static int jnN = 0, jnCap = 0;
static char *jarena = NULL; static size_t jaUsed = 0;
static const char *jp;
static void jfree() { if (jdoc) free(jdoc); if (jarena) free(jarena); jdoc = NULL; jarena = NULL; jnN = jnCap = 0; jaUsed = 0; }
static int jnew(uint8_t t) {
  if (jnN == jnCap) {
    int c = jnCap ? jnCap * 2 : 512;
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
static const char *jstring() {              // at the opening quote; unescapes into the arena
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
static float jflt(int n, float d) { return n >= 0 && jdoc[n].t == J_NUM ? jdoc[n].n : d; }
static const char *jgs(int o, const char *k, const char *d = "") { return jstr(jget(o, k), d); }

// ---- editions: list, load, state
struct EdRef { char slug[32]; char name[48]; bool builtin; };
static EdRef edList[ED_MAX]; static int edN = 0, edCur = -1;
static bool sdUp = false, edScanned = false;
static int edRoot = -1, *cardN = NULL, nCards = 0;
static const char *secId[24], *secTitle[24]; static int nSecs = 0;
static uint8_t *lBox = NULL; static int32_t *lDay = NULL; static uint32_t *edTick = NULL;

static bool slugOk(const char *s) {          // lowercase letters, digits, dashes; never "!"
  if (!*s || strlen(s) > 30) return false;
  for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false;
  return true;
}
static char *readAll(fs::FS &fs, const String &path, size_t cap = 1 << 20) {
  File f = fs.open(path, "r");
  if (!f) return NULL;
  size_t n = f.size();
  if (n > cap) { f.close(); return NULL; }
  char *b = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (b) { f.read((uint8_t *)b, n); b[n] = 0; }
  f.close();
  return b;
}
static void sdMount() {
  if (sdUp) return;
  SD_MMC.setPins(39, 41, 40);                // CLK, CMD, D0 (Waveshare 1.54 V2)
  sdUp = SD_MMC.begin("/sdcard", true);      // 1-bit mode
}
static void sdUnmount() { if (sdUp) SD_MMC.end(); sdUp = false; }
static void builtinOnly();
static int builtinIdx(const char *slug);
static String edSdLeftOut;                     // v0.25.2: packs on the card left out at the last scan (not drawable)
static void scanEditions() {
  edScanned = true;
  builtinOnly();
  edSdLeftOut = "";
  sdMount();
  if (!sdUp) return;
  File root = SD_MMC.open("/editions");
  if (!root || !root.isDirectory()) return;
  for (File d = root.openNextFile(); d && edN < ED_MAX; d = root.openNextFile()) {
    if (!d.isDirectory()) continue;
    String slug = d.name(); int sl = slug.lastIndexOf('/'); if (sl >= 0) slug = slug.substring(sl + 1);
    if (!slugOk(slug.c_str()) || builtinIdx(slug.c_str()) >= 0) continue;   // built-in packs win
    char *e = readAll(SD_MMC, "/editions/" + slug + "/edition.json", 64 * 1024);
    if (!e) continue;
    String name = jsonField(String(e), "name");
    char *c = readAll(SD_MMC, "/editions/" + slug + "/cards.json");   // v0.25.2: drawable with these fonts?
    bool drawable = !c || packDrawable(e, c);   // no cards file: the reader says so when it is opened
    free(e); free(c);
    if (!drawable) { if (edSdLeftOut.length()) edSdLeftOut += ", "; edSdLeftOut += name.length() ? name : slug; continue; }
    EdRef &r = edList[edN++];
    strlcpy(r.slug, slug.c_str(), sizeof r.slug);
    strlcpy(r.name, name.length() ? name.c_str() : slug.c_str(), sizeof r.name);
    r.builtin = false;
  }
}
static int edFind(const char *slug) { for (int i = 0; i < edN; i++) if (!strcmp(edList[i].slug, slug)) return i; return -1; }

static bool lfsMount() { static bool up = false; if (!up) up = LittleFS.begin(true, "/lfs", 5, "spiffs"); return up; }
static const char *baseOf(const char *slug);
static String statePath() { return String("/ed-") + baseOf(edList[edCur].slug) + ".st"; }   // one progress per edition, all languages
static void saveState() {
  if (edCur < 0 || !lfsMount()) return;
  File f = LittleFS.open(statePath(), "w");
  if (!f) return;
  for (int i = 0; i < nCards; i++) {
    const char *id = jgs(cardN[i], "id");
    if (lDay[i]) f.printf("l %s %d %ld\n", id, lBox[i], (long)lDay[i]);
    if (edTick[i]) f.printf("t %s %lx\n", id, (unsigned long)edTick[i]);
  }
  f.close();
}
static void loadState() {
  memset(lBox, 0, nCards); memset(lDay, 0, nCards * 4); memset(edTick, 0, nCards * 4);
  if (!lfsMount()) return;
  File f = LittleFS.open(statePath(), "r");
  if (!f) return;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    int a = l.indexOf(' '), b = l.indexOf(' ', a + 1); if (a != 1 || b < 0) continue;
    String id = l.substring(2, b), rest = l.substring(b + 1);
    int i = -1; for (int k = 0; k < nCards; k++) if (id == jgs(cardN[k], "id")) { i = k; break; }
    if (i < 0) continue;                    // card gone from this version of the pack
    if (l[0] == 't') edTick[i] = strtoul(rest.c_str(), NULL, 16);
    else { int sp = rest.indexOf(' '); lBox[i] = constrain(rest.toInt(), 1, 5); lDay[i] = rest.substring(sp + 1).toInt(); }
  }
  f.close();
}

extern const char EZ_EDITION[], EZ_CARDS[], AT_EDITION[], AT_CARDS[];
extern const char VU_EDITION[], VU_CARDS[], YO_EDITION[], YO_CARDS[], CA_EDITION[], CA_CARDS[];
extern const char WF_EDITION[], WF_CARDS[], WFD_EDITION[], WFD_CARDS[], WFF_EDITION[], WFF_CARDS[];
extern const char GU_EDITION[], GU_CARDS[];   // v0.26.0
struct Builtin { const char *slug, *name, *edition, *cards, *lang, *base; };   // base: set on a language variant
static const Builtin BUILTIN[] = {             // compiled into the firmware: no card needed
  { "atlantidia-lite", "Atlantid!a Lite", AT_EDITION, AT_CARDS },
  { "vulcania-lite",   "Vulcan!a Lite",   VU_EDITION, VU_CARDS },
  { "yogia-lite",      "Yog!a Lite",      YO_EDITION, YO_CARDS },
  { "catlas",          "C@tlas",          CA_EDITION, CA_CARDS },
  { "guineapedia-lite", "Guineapedi@ Lite", GU_EDITION, GU_CARDS },   // v0.26.0
  { "waterfacts-lite",    "W@terfacts! Lite", WF_EDITION,  WF_CARDS,  "en", NULL },
  { "waterfacts-lite-de", "W@terfacts! Lite", WFD_EDITION, WFD_CARDS, "de", "waterfacts-lite" },
  { "waterfacts-lite-fr", "W@terfacts! Lite", WFF_EDITION, WFF_CARDS, "fr", "waterfacts-lite" },
  { "edition-zero",    "Edit!on Zero",    EZ_EDITION, EZ_CARDS },
};
#define N_BUILTIN (int)(sizeof BUILTIN / sizeof BUILTIN[0])
static int builtinIdx(const char *slug) { for (int i = 0; i < N_BUILTIN; i++) if (!strcmp(BUILTIN[i].slug, slug)) return i; return -1; }
// ---- languages: an edition may ship as a base pack plus language variants with the same card ids.
// The shelf shows the base once, pointing at the variant in the preferred language; progress is shared.
static String edLang() { return prefs.getString("edLang", L("en", "fr")); }
static const char *baseOf(const char *slug) {
  int b = builtinIdx(slug); if (b < 0) return slug;
  return BUILTIN[b].base ? BUILTIN[b].base : BUILTIN[b].slug;
}
static const char *langOf(const char *slug) { int b = builtinIdx(slug); return b >= 0 && BUILTIN[b].lang ? BUILTIN[b].lang : ""; }
// ---- v0.25.2: a pack the fonts cannot draw (a Japanese pack before kana and kanji are in them) is left out of the
// shelf and named on the Editions tile, never shown as a screen of empty boxes (contract v13, the X4's rule).
// "Cannot draw": more than 1 in 100 of its visible characters have no glyph. In this build no pack misses one.
static void glyphsMissing(const char *s, int &total, int &missing) {
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    uint32_t cp = *p++; int more = 0;
    if (cp >= 0xF0) { cp &= 0x07; more = 3; } else if (cp >= 0xE0) { cp &= 0x0F; more = 2; } else if (cp >= 0xC0) { cp &= 0x1F; more = 1; }
    while (more-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    if (cp <= 32) continue;
    total++;
    if (cp > 0xFFFF || ek_glyph_index((uint16_t)cp) < 0) missing++;
  }
}
static bool packDrawable(const char *edition, const char *cards) {
  int total = 0, missing = 0;
  glyphsMissing(edition, total, missing); glyphsMissing(cards, total, missing);
  return missing * 100 <= total;
}
static bool builtinOk(int i) {                 // worked out once per wake
  static int8_t ok[N_BUILTIN] = { 0 };
  if (!ok[i]) ok[i] = packDrawable(BUILTIN[i].edition, BUILTIN[i].cards) ? 1 : 2;
  return ok[i] == 1;
}
static String edLeftOut() {                    // every pack left out, by name (and language), for the Editions tile
  String r;
  for (int i = 0; i < N_BUILTIN; i++) if (!builtinOk(i)) {
    if (r.length()) r += ", ";
    r += BUILTIN[i].name; if (BUILTIN[i].lang) { r += " ("; r += BUILTIN[i].lang; r += ")"; }
  }
  if (edSdLeftOut.length()) { if (r.length()) r += ", "; r += edSdLeftOut; }
  return r;
}
static int langCount(const char *slug) {             // how many languages this edition has
  const char *base = baseOf(slug); int n = 0;
  for (int i = 0; i < N_BUILTIN; i++) if ((!strcmp(BUILTIN[i].slug, base) || (BUILTIN[i].base && !strcmp(BUILTIN[i].base, base))) && builtinOk(i)) n++;
  return n;
}
static const char *langVariant(const char *base, const char *lang) {   // the family member in that language, or NULL
  for (int i = 0; i < N_BUILTIN; i++) {
    bool fam = !strcmp(BUILTIN[i].slug, base) || (BUILTIN[i].base && !strcmp(BUILTIN[i].base, base));
    if (fam && BUILTIN[i].lang && !strcmp(BUILTIN[i].lang, lang) && builtinOk(i)) return BUILTIN[i].slug;
  }
  return NULL;
}
static const char *nextLangSlug(const char *slug) {   // the next language, in shelf order, wrapping
  const char *base = baseOf(slug); int cur = builtinIdx(slug), first = -1, next = -1; bool after = false;
  for (int i = 0; i < N_BUILTIN; i++) {
    bool fam = !strcmp(BUILTIN[i].slug, base) || (BUILTIN[i].base && !strcmp(BUILTIN[i].base, base));
    if (!fam || !builtinOk(i)) continue;
    if (first < 0) first = i;
    if (after && next < 0) next = i;
    if (i == cur) after = true;
  }
  return BUILTIN[next >= 0 ? next : first].slug;
}
static void builtinOnly() {
  edN = 0; String pref = edLang();
  for (int i = 0; i < N_BUILTIN; i++) {
    if (BUILTIN[i].base) continue;                   // language variants live behind their base
    const char *v = langVariant(BUILTIN[i].slug, pref.c_str());
    if (!v && !builtinOk(i)) {                      // v0.25.2: the base cannot be drawn: a drawable variant, or nothing
      for (int k = 0; k < N_BUILTIN && !v; k++) if (BUILTIN[k].base && !strcmp(BUILTIN[k].base, BUILTIN[i].slug) && builtinOk(k)) v = BUILTIN[k].slug;
      if (!v) continue;
    }
    EdRef &r = edList[edN++];
    strlcpy(r.slug, v ? v : BUILTIN[i].slug, sizeof r.slug); strlcpy(r.name, BUILTIN[i].name, sizeof r.name); r.builtin = true;
  }
}
static bool loadEdition(int idx) {
  jfree(); free(cardN); free(lBox); free(lDay); free(edTick);
  cardN = NULL; lBox = NULL; lDay = NULL; edTick = NULL; nCards = 0; nSecs = 0; edCur = -1;
  char *e = NULL, *c = NULL;
  if (edList[idx].builtin) {
    int b = builtinIdx(edList[idx].slug); if (b < 0) return false;
    e = (char *)BUILTIN[b].edition; c = (char *)BUILTIN[b].cards;
  } else {
    sdMount();
    if (!sdUp) return false;
    e = readAll(SD_MMC, String("/editions/") + edList[idx].slug + "/edition.json");
    c = readAll(SD_MMC, String("/editions/") + edList[idx].slug + "/cards.json");
    if (!e || !c) { free(e); free(c); return false; }
  }
  size_t need = strlen(e) + strlen(c) + 64;
  jarena = (char *)heap_caps_malloc(need + need / 2, MALLOC_CAP_SPIRAM);   // unescaped text never grows
  bool ok = jarena != NULL;
  int ed = ok ? jparse(e) : -1, cs = ok ? jparse(c) : -1;
  if (!edList[idx].builtin) { free(e); free(c); }
  if (ed < 0 || cs < 0 || jdoc[ed].t != J_OBJ || jdoc[cs].t != J_ARR) { jfree(); return false; }
  edRoot = ed; edCur = idx;
  nCards = jdoc[cs].count;
  cardN = (int *)malloc(max(nCards, 1) * sizeof(int));
  lBox = (uint8_t *)malloc(max(nCards, 1)); lDay = (int32_t *)malloc(max(nCards, 1) * 4); edTick = (uint32_t *)malloc(max(nCards, 1) * 4);
  int k = 0; for (int n = jdoc[cs].child; n >= 0; n = jdoc[n].next) cardN[k++] = n;
  // sections, sorted by "order"
  int ss = jget(ed, "sections");
  float ord[24];
  for (int n = ss >= 0 ? jdoc[ss].child : -1; n >= 0 && nSecs < 23; n = jdoc[n].next) {
    secId[nSecs] = jgs(n, "id"); secTitle[nSecs] = jgs(n, "title", secId[nSecs]); ord[nSecs] = jflt(jget(n, "order"), nSecs + 1); nSecs++;
  }
  for (int i = 1; i < nSecs; i++) for (int j = i; j > 0 && ord[j] < ord[j - 1]; j--) {
    std::swap(ord[j], ord[j - 1]); std::swap(secId[j], secId[j - 1]); std::swap(secTitle[j], secTitle[j - 1]);
  }
  // cards whose section is not listed go to a last "More" section
  for (int i = 0; i < nCards; i++) {
    const char *s = jgs(cardN[i], "section"); bool found = false;
    for (int j = 0; j < nSecs; j++) if (!strcmp(secId[j], s)) found = true;
    if (!found) { secId[nSecs] = "\x01more"; secTitle[nSecs] = "More"; nSecs++; break; }
  }
  loadState();
  smallText = prefs.getBool("edSmall", false);
  return true;
}
static bool cardInSec(int i, int s) {
  const char *cs = jgs(cardN[i], "section");
  if (strcmp(secId[s], "\x01more")) return !strcmp(cs, secId[s]);
  for (int j = 0; j < nSecs; j++) if (!strcmp(secId[j], cs)) return false;
  return true;
}
static bool isType(int i, const char *t) { return !strcmp(jgs(cardN[i], "type"), t); }

// ---- days (for Leitner): real calendar days, or a session count if the clock was never set
static int32_t civilDays(int y, int m, int d) {             // days since 1970-01-01
  y -= m <= 2; int era = (y >= 0 ? y : y - 399) / 400; int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static int32_t today() {
  if (timeValid()) { time_t e = time(NULL); struct tm t; localtime_r(&e, &t); return civilDays(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday); }
  return 1 + prefs.getInt("sess", 0);       // no clock: each start counts as a day
}
static const uint8_t LBOX_DAYS[6] = { 0, 1, 2, 4, 8, 16 };
static bool isDue(int i) {
  if (!isType(i, "prompt")) return false;
  if (!lDay[i]) return true;                // never seen
  return today() - lDay[i] >= LBOX_DAYS[max(1, (int)lBox[i])];
}
static void rate(int i, bool ok) {
  int b = max(1, (int)lBox[i]);
  lBox[i] = ok ? min(5, b + 1) : 1; lDay[i] = today();
  saveState();
}
static int pickPotd() {                     // today's prompt: a due one from the lowest box
  int32_t d = today(); int best = -1, bestBox = 9, ties = 0;
  for (int i = 0; i < nCards; i++) if (isDue(i)) {
    int b = max(1, (int)lBox[i]);
    if (b < bestBox) { bestBox = b; best = i; ties = 1; }
    else if (b == bestBox && (uint32_t)(d * 2654435761u + i) % ++ties == 0) best = i;
  }
  if (best >= 0) return best;
  int n = 0; for (int i = 0; i < nCards; i++) if (isType(i, "prompt")) n++;
  if (!n) return -1;
  int k = d % n; for (int i = 0; i < nCards; i++) if (isType(i, "prompt") && !k--) return i;
  return -1;
}

// ---- reader state
enum { RV_LIBRARY, RV_SPLASH, RV_SECTIONS, RV_CARD, RV_DOC, RV_CATLIST, RV_CATQR, RV_VCARD };
static bool catMode = false;                   // the reader is showing the InvisibleChaos catalog
// generated from the hub's dial (invisiblechaos.netlify.app, index.html STATIONS, V29) + pinned entries
struct Station { const char *freq, *code, *name, *url, *tag; };
static const Station STATIONS[] = {
#if EK_INVISIBLEOCEAN
  { "103.9", "EDIT", "E!nkster Editions", "https://einkster-editions.netlify.app/", "" },
  { "96.0", "OCEAN", "!nvisibleOcean", "https://invisibleocean.netlify.app/", "" },
  { "99.1", "VOLCANO", "VolcanoBro", "https://volcanobro.netlify.app/", "" },
  { "101.3", "MET", "MeteoBro", "https://meteobro.netlify.app/", "" },
  { "105.1", "FPN", "Fipness", "https://fipness.netlify.app/", "" },
  { "", "IC", "InvisibleChaos", "https://invisiblechaos.netlify.app/", "hub" },
#else
  { "", "CV", "Resume", "https://cv-julien-le-tyrant.netlify.app/", "" },          // v0.25.2: right after the business card
  { "", "CV-ZH", "Resume, Chinese", "https://cv-julien-zh.netlify.app/", "" },
  { "103.9", "EDIT", "E!nkster Editions", "https://einkster-editions.netlify.app/", "" },
  { "", "IC", "InvisibleChaos", "https://invisiblechaos.netlify.app/", "hub" },
  { "86.0", "QLC", "Quellcode", "https://quellcode-berlin.netlify.app/", "" },
  { "88.0", "TUNE", "TunerBro", "https://tunerbro.netlify.app/", "" },
  { "89.6", "DDD", "DerDieDas Bro", "https://derdiedas-bro.netlify.app/", "" },
  { "91.3", "WASSER", "Wasserbro", "https://wasserbro.netlify.app/", "" },
  { "93.4", "DIY", "La Carte", "https://ic-kitchen.netlify.app/", "" },
  { "94.3", "RDZ", "Radzeugnis", "https://radzeugnis.netlify.app/", "" },
  { "96.0", "OCEAN", "!nvisibleOcean", "https://invisibleocean.netlify.app/", "" },
  { "97.2", "SPL", "Spielhalle", "https://invisiblechaos.netlify.app/games.html", "" },
  { "99.1", "VOLCANO", "VolcanoBro", "https://volcanobro.netlify.app/", "" },
  { "101.3", "MET", "MeteoBro", "https://meteobro.netlify.app/", "" },
  { "105.1", "FPN", "Fipness", "https://fipness.netlify.app/", "" },
  { "106.5", "KFS", "Kaffeesatz", "https://kaffeesatz.netlify.app/", "" },
  { "108.9", "ALT", "Altname", "https://altname.netlify.app/", "" },
  { "112.0", "CIM", "ChaosInMotion", "https://chaosinmotion.netlify.app/", "" },
  { "114.2", "KMD", "Kamerad", "https://kamerad.netlify.app/", "" },
  { "117.1", "NBSP", "Insécable", "https://insecable.netlify.app/", "" },
  { "118.8", "LUC", "Lucible", "https://lucible.netlify.app/", "" },
  { "120.0", "HUD", "Field HUD", "https://recumbenthud.netlify.app/", "field" },
  { "121.5", "SURV", "The Survivalist", "https://thesurvivalist.netlify.app/", "" },
  { "123.7", "RDF", "Radfunk!", "https://radfunk.netlify.app/", "" },
#endif
};
#define N_STATIONS (int)(sizeof STATIONS / sizeof STATIONS[0])

enum { DK_SECTION, DK_REVIEW, DK_POTD };
static bool rdOn = false;
static int rv = RV_SPLASH, rdCursor = 0, dkKind = DK_SECTION, dkSec = 0, dkPos = 0, rdPage = 0;
static bool revealed = false;
static int deck[256], deckN = 0;
static String docTitle, docBody;
RTC_DATA_ATTR static int8_t  rtcRdOn, rtcRdView, rtcRdKind, rtcRdRev;
RTC_DATA_ATTR static int16_t rtcRdCursor, rtcRdSec, rtcRdPos, rtcRdPage, rtcPotdCard;
RTC_DATA_ATTR static char    rtcRdSlug[32];
RTC_DATA_ATTR static int32_t rtcPotdDay;
static bool potdOn() { return prefs.getBool("potd", false); }

static void buildDeck() {
  deckN = 0;
  if (dkKind == DK_SECTION) { for (int i = 0; i < nCards && deckN < 256; i++) if (cardInSec(i, dkSec)) deck[deckN++] = i; }
  else if (dkKind == DK_REVIEW) {
    for (int b = 1; b <= 5; b++) for (int i = 0; i < nCards && deckN < 256; i++)
      if (isDue(i) && max(1, (int)lBox[i]) == b) deck[deckN++] = i;
  } else { int p = rtcPotdCard >= 0 && rtcPotdCard < nCards ? rtcPotdCard : pickPotd(); if (p >= 0) deck[deckN++] = p; }
  dkPos = constrain(dkPos, 0, max(0, deckN - 1));
}

// ---- text: UTF-8 with **bold** (drawn underlined: this font is already bold) and \n
#define RB_MAX 6000
static uint16_t rbCp[RB_MAX]; static uint8_t rbFl[RB_MAX]; static int rbN = 0;
enum { K_TITLE, K_TEXT, K_GAP, K_RULE, K_ITEM, K_ITEMC, K_SMALL, K_QR };
struct RLine { uint16_t a, b; uint8_t kind; int8_t item; uint8_t h; };
static RLine rl[400]; static int rlN = 0;
static int pgStart[64], pgN = 0;
static uint8_t qrM[57 * 57]; static int qrSize = 0, qrScale = 0;
static void qrGrab(esp_qrcode_handle_t q) {
  qrSize = esp_qrcode_get_size(q); if (qrSize > 57) { qrSize = 0; return; }
  for (int y = 0; y < qrSize; y++) for (int x = 0; x < qrSize; x++) qrM[y * qrSize + x] = esp_qrcode_get_module(q, x, y);
}
static int textWidth2(const EkFont &f, const String &s);
static const EkFont &kFont(int k) {
  if (k == K_SMALL || k == K_ITEM || k == K_ITEMC) return EK_SMALL;   // checklists are always compact
  if (smallText && k == K_TEXT) return EK_SMALL;
  return EK_MID;
}
static int cpW(const EkFont &f, int a, int b) { int w = 0; for (int i = a; i < b; i++) w += glyphW(f, rbCp[i]); return w; }
static void pushLine(int a, int b, int kind, int item, int h) {
  if (rlN < 400) rl[rlN++] = { (uint16_t)a, (uint16_t)b, (uint8_t)kind, (int8_t)item, (uint8_t)h };
}
// append text to the buffer and wrap it into lines of the given kind
static void addText(const char *s, int kind, int width, int item = -1, int contKind = -1) {
  const EkFont &f = kFont(kind);
  int start = rbN; bool bold = false;
  const uint8_t *p = (const uint8_t *)s;
  static uint16_t tmp[1];
  while (*p && rbN < RB_MAX - 1) {
    if (p[0] == '*' && p[1] == '*') { bold = !bold; p += 2; continue; }
    const uint8_t *q = p; int len = 1;
    if (*q >= 0xF0) len = 4; else if (*q >= 0xE0) len = 3; else if (*q >= 0xC0) len = 2;
    char ch[5] = {0}; for (int i = 0; i < len && q[i]; i++) ch[i] = q[i];
    p += strlen(ch) ? strlen(ch) : 1;
    int n = decodeUtf8c(ch, tmp, 2);
    if (!n) continue;
    rbCp[rbN] = tmp[0]; rbFl[rbN] = bold; rbN++;
  }
  int i = start, first = 1, h = f.height;
  while (i < rbN) {
    if (rbCp[i] == '\n') {                  // blank line = small gap; single \n = line break
      if (i + 1 < rbN && rbCp[i + 1] == '\n') { pushLine(i, i, K_GAP, -1, 9); i += 2; continue; }
      i++; continue;
    }
    int a = i, w = 0, lastSp = -1;
    while (i < rbN && rbCp[i] != '\n') {
      int gw = glyphW(f, rbCp[i]);
      if (w + gw > width && i > a) break;
      if (rbCp[i] == ' ') lastSp = i;
      w += gw; i++;
    }
    int b = i;
    if (i < rbN && rbCp[i] != '\n' && lastSp > a) { b = lastSp; i = lastSp + 1; }
    else if (i < rbN && rbCp[i] == ' ') i++;
    pushLine(a, b, first || contKind < 0 ? kind : contKind, item, h);
    first = 0;
    if (i < rbN && rbCp[i] == '\n' && !(i + 1 < rbN && rbCp[i + 1] == '\n')) i++;
  }
}
#define BODY_Y  22
#define BODY_H  156
static void paginate() {
  pgN = 0; int y = 0;
  pgStart[pgN++] = 0;
  for (int i = 0; i < rlN; i++) {
    int need = rl[i].h;                     // keep a checklist item whole, and a title with its next line
    if (rl[i].kind == K_ITEM) for (int j = i + 1; j < rlN && rl[j].kind == K_ITEMC && rl[j].item == rl[i].item; j++) need += rl[j].h;
    if (rl[i].kind == K_TITLE && i + 1 < rlN && rl[i + 1].kind != K_TITLE) need += rl[i + 1].h;
    if (rl[i].kind == K_SMALL && (i == 0 || rl[i - 1].kind != K_SMALL))      // a wrapped link stays together
      for (int j = i + 1; j < rlN && rl[j].kind == K_SMALL; j++) need += rl[j].h;
    if (need > BODY_H) need = rl[i].h;
    if (y + need > BODY_H && y > 0 && pgN < 64) { pgStart[pgN++] = i; y = 0; }
    if (y == 0 && i > 0 && rl[i].kind == K_GAP) continue;     // no gap at the top of a page
    y += rl[i].h;
  }
}
static int pageEnd(int p) { return p + 1 < pgN ? pgStart[p + 1] : rlN; }

static void layoutCard(int ci) {
  rbN = 0; rlN = 0; qrSize = 0;
  int c = cardN[ci];
  const char *type = jgs(c, "type");
  if (!strcmp(type, "prompt")) {
    String lab = String(L("prompt  \xC2\xB7  box ", "question  \xC2\xB7  bo\xC3\xAE" "te ")) + String(max(1, (int)lBox[ci])) + "/5";
    addText(lab.c_str(), K_SMALL, 184);
  }
  int t0 = rlN; addText(jgs(c, "title", "untitled"), K_TITLE, 184);
  for (int i = t0; i < rlN; i++) rl[i].h = EK_MID.height + 6;
  if (!strcmp(type, "ref")) addText(jgs(c, "body"), K_TEXT, 184);
  else if (!strcmp(type, "checklist")) {
    int it = jget(c, "items"), k = 0;
    for (int n = it >= 0 ? jdoc[it].child : -1; n >= 0 && k < 32; n = jdoc[n].next, k++) addText(jstr(n), K_ITEM, 160, k, K_ITEMC);
  } else if (!strcmp(type, "prompt")) {
    addText(jgs(c, "question"), K_TEXT, 184);
    if (revealed) { pushLine(0, 0, K_RULE, -1, 11); addText(jgs(c, "answer"), K_TEXT, 184); }
  } else if (!strcmp(type, "qr")) {
    esp_qrcode_config_t cfg = { qrGrab, 10, ESP_QRCODE_ECC_MED };
    if (esp_qrcode_generate(&cfg, jgs(c, "url")) == ESP_OK && qrSize) {
      int used = 0; for (int i = 0; i < rlN; i++) used += rl[i].h;
      int avail = BODY_H - used - 4;        // the code gets the page; the link follows it
      qrScale = min(4, avail / (qrSize + 2));
      if (qrScale < 2) qrScale = 2;         // very long links spill onto their own page
      pushLine(0, 0, K_QR, -1, (qrSize + 2) * qrScale + 2);
    } else addText("(link too long for a code)", K_SMALL, 184);
    addText(jgs(c, "url"), K_SMALL, 184);
    pushLine(0, 0, K_GAP, -1, 9);
    addText(jgs(c, "text"), K_TEXT, 184);
  } else addText((String("(card type \"") + type + "\" is not supported here)").c_str(), K_SMALL, 184);
  paginate();
}
static void layoutDoc() {
  rbN = 0; rlN = 0; qrSize = 0;
  addText(docTitle.c_str(), K_TITLE, 184);
  for (int i = 0; i < rlN; i++) rl[i].h = EK_MID.height + 6;
  addText(docBody.c_str(), K_TEXT, 184);
  paginate();
}

// ---- drawing
static void glyphInk(const EkFont &f, int x, int y, uint16_t cp, bool ink) {
  int i = ek_glyph_index(cp);
  if (i < 0) {                              // missing glyph: a hollow box, never a crash
    if (cp == ' ' || cp == '\n') return;
    int w = glyphW(f, cp);
    rect(x + 1, y + 3, w - 3, 1, ink); rect(x + 1, y + f.height - 5, w - 3, 1, ink);
    rect(x + 1, y + 3, 1, f.height - 7, ink); rect(x + w - 3, y + 3, 1, f.height - 7, ink);
    return;
  }
  const EkGlyph &g = f.glyphs[i]; int bpr = (g.width + 7) / 8;
  const uint8_t *b = f.bits + g.offset;
  for (int r = 0; r < f.height; r++) for (int c = 0; c < g.width; c++)
    if (b[r * bpr + (c >> 3)] & (0x80 >> (c & 7))) px(x + c, y + r, ink);
}
static int textLeft(const EkFont &f, int x, int y, const String &s, bool ink = true, int maxW = 999) {
  static uint16_t t[128]; int n = decodeUtf8(s, t, 128), w = 0;
  for (int i = 0; i < n; i++) {
    int gw = glyphW(f, t[i]);
    if (w + gw > maxW) { for (int k = 0; k < 3; k++) rect(x + w + 2 + k * 4, y + f.ascent - 2, 2, 2, ink); break; }
    glyphInk(f, x + w, y, t[i], ink); w += gw;
  }
  return w;
}
static int textWidth2(const EkFont &f, const String &s) { static uint16_t t[128]; int n = decodeUtf8(s, t, 128), w = 0; for (int i = 0; i < n; i++) w += glyphW(f, t[i]); return w; }
static void drawLineAt(int li, int y) {
  const RLine &L = rl[li];
  const EkFont &f = kFont(L.kind);
  int x = 8;
  if (L.kind == K_ITEM || L.kind == K_ITEMC) x = 32;
  if (L.kind == K_SMALL) x = 100 - cpW(f, L.a, L.b) / 2;
  for (int i = L.a; i < L.b; i++) {
    int gw = glyphW(f, rbCp[i]);
    glyphInk(f, x, y, rbCp[i], true);
    if (rbFl[i]) rect(x, y + f.ascent + 2, gw, 2, true);     // **bold** = underline on this bold-only font
    x += gw;
  }
  if (L.kind == K_TITLE) rect(8, y + f.height + 2, max(24, cpW(f, L.a, L.b)), 2, true);
}
static int itemTop[32], itemBot[32];
static void drawBodyPage(int ci) {
  for (int k = 0; k < 32; k++) itemTop[k] = itemBot[k] = -1;
  int y = BODY_Y;
  for (int i = pgStart[rdPage]; i < pageEnd(rdPage); i++) {
    RLine &L = rl[i];
    if (y == BODY_Y && i > 0 && L.kind == K_GAP) continue;
    if (L.kind == K_RULE) rect(40, y + 5, 120, 1, true);
    else if (L.kind == K_QR) {
      int s = qrScale, side = (qrSize + 2) * s, x0 = 100 - side / 2 + s, y0 = y + s;
      for (int r = 0; r < qrSize; r++) for (int c = 0; c < qrSize; c++) if (qrM[r * qrSize + c]) rect(x0 + c * s, y0 + r * s, s, s, true);
    } else if (L.kind != K_GAP) drawLineAt(i, y);
    if ((L.kind == K_ITEM || L.kind == K_ITEMC) && L.item >= 0 && ci >= 0) {
      if (itemTop[L.item] < 0) itemTop[L.item] = y;
      itemBot[L.item] = y + L.h;
      if (L.kind == K_ITEM) {                // the checkbox
        bool on = edTick[ci] & (1UL << L.item);
        int by = y + 1;
        rect(8, by, 16, 16, true); if (!on) rect(10, by + 2, 12, 12, false);
        if (on) {                            // filled box with a white tick
          int cy = by - 4;
          rect(10, by + 2, 12, 12, true);
          for (int k = 0; k < 6; k++) { px(12 + k, cy + 11 + k / 2, false); px(12 + k, cy + 12 + k / 2, false); }
          for (int k = 0; k < 7; k++) { px(17 + k / 2 + 1, cy + 13 - k, false); px(18 + k / 2, cy + 13 - k, false); }
        }
      }
    }
    y += L.h;
  }
}
static void rdHeader(const String &left, const String &right) {
  int rw = right.length() ? textWidth2(EK_SMALL, right) : 0;
  textLeft(EK_SMALL, 8, 1, left, true, 184 - rw - 10);
  if (rw) textLeft(EK_SMALL, 192 - rw, 1, right);
  rect(8, 18, 184, 1, true);
}
static void rdFooter(const String &s) { if (s.length()) textAt(EK_SMALL, 100, 182, s); }
static void bookIcon(int cx, int cy) {
  for (int k = 0; k < 3; k++) { rect(cx - 30, cy - 18 + k, 28, 1, true); rect(cx + 2, cy - 18 + k, 28, 1, true); }
  rect(cx - 30, cy - 18, 2, 36, true); rect(cx + 28, cy - 18, 2, 36, true);
  rect(cx - 1, cy - 20, 2, 40, true); rect(cx - 30, cy + 16, 60, 2, true);
  for (int k = 0; k < 4; k++) { rect(cx - 24, cy - 10 + k * 7, 18, 2, true); rect(cx + 6, cy - 10 + k * 7, 18, 2, true); }
}
static String menuLabel(int i, String &right) {
  right = "";
  if (i < nSecs) { int n = 0; for (int k = 0; k < nCards; k++) if (cardInSec(k, i)) n++; right = String(n); return secTitle[i]; }
  i -= nSecs;
  if (i == 0) { int n = 0; for (int k = 0; k < nCards; k++) if (isDue(k)) n++; right = String(n); return L("review", "r\xC3\xA9" "vision"); }
  if (i == 1) { right = potdOn() ? L("on", "oui") : L("off", "non"); return L("daily prompt", "question du jour"); }
  if (i == 2) { right = smallText ? L("small", "petit") : L("large", "grand"); return L("text size", "taille du texte"); }
  if (i == 3 && langCount(edList[edCur].slug) > 1) { right = langOf(edList[edCur].slug); right.toUpperCase(); return L("language", "langue"); }
  return L("about", "\xC3\xA0" " propos");
}
static int menuCount() { return nSecs + 4 + (langCount(edList[edCur].slug) > 1 ? 1 : 0); }
static void drawList(int count, int cursor, std::function<String(int, String &)> label) {
  const int ROW = 26, ROWS = count > 6 ? 5 : 6;   // leave room for the page counter
  int first = cursor / ROWS * ROWS;
  for (int r = 0; r < ROWS && first + r < count; r++) {
    int i = first + r, y = BODY_Y + r * ROW; bool sel = i == cursor;
    if (sel) rect(4, y, 192, ROW - 2, true);
    String right; String l = label(i, right);
    int rw = right.length() ? textWidth2(EK_SMALL, right) : 0;
    int room = 176 - rw - 8;                    // v0.24.0: a label too wide for the large type is set in the small one, whole, instead of cut
    if (textWidth2(EK_MID, l) <= room) textLeft(EK_MID, 10, y + 1, l, !sel, room); else textLeft(EK_SMALL, 10, y + 5, l, !sel, room);
    if (rw) textLeft(EK_SMALL, 190 - rw, y + 5, right, !sel);
  }
  if (count > ROWS) textAt(EK_SMALL, 100, 158, String(first / ROWS + 1) + "/" + String((count + ROWS - 1) / ROWS));
}
static void catQr(int i) {                     // one station, as large as the screen allows
  const Station &st = STATIONS[i];
  String f = st.freq[0] ? String(st.freq) + "  " : String("");
  String top = f + st.code + "  \xC2\xB7  " + st.name;
  if (textWidth2(EK_SMALL, top) > 192) top = f + st.name;          // too wide: drop the code
  textAt(EK_SMALL, 100, 1, top);
  String u = st.url;
  if (u.startsWith("https://")) u = u.substring(8);
  if (u.length() && u[u.length() - 1] == '/') u = u.substring(0, u.length() - 1);
  String u1 = u, u2 = "";
  if (textWidth2(EK_SMALL, u) > 192) { int k = u.indexOf('.'); u1 = u.substring(0, k); u2 = u.substring(k); }
  int boxH = u2.length() ? 144 : 160;          // two address lines: a slightly smaller code
  esp_qrcode_config_t cfg = { qrGrab, 10, ESP_QRCODE_ECC_MED };
  qrSize = 0;
  if (esp_qrcode_generate(&cfg, st.url) == ESP_OK && qrSize) {
    int sc = min(6, boxH / (qrSize + 2)), side = qrSize * sc;
    int x0 = 100 - side / 2, y0 = 20 + (boxH - side) / 2;
    for (int r = 0; r < qrSize; r++) for (int c = 0; c < qrSize; c++) if (qrM[r * qrSize + c]) rect(x0 + c * sc, y0 + r * sc, sc, sc, true);
  } else textBlock("link too long for a code", 8, 60, 184, 80, false);
  if (u2.length()) { textAt(EK_SMALL, 100, 165, u1); textAt(EK_SMALL, 100, 182, u2); }
  else textAt(EK_SMALL, 100, 182, u1);
}

// ---- business card: the first row of the catalog, set apart. Details come from the EK_CARD build secret
// ("name|email|phone"), never from the source: no secret, no card (the InvisibleOcean build has none).
#ifndef EK_CARD
#define EK_CARD ""
#endif
static int cardPg = 0;                            // 0 both (contact card), 1 e-mail, 2 WhatsApp
static bool cardOn() { return strchr(EK_CARD, '|') != NULL; }
static String cardField(int k) {                  // 0 name, 1 email, 2 phone
  String c = EK_CARD; int a = 0;
  for (int i = 0; i < k; i++) { a = c.indexOf('|', a); if (a < 0) return ""; a++; }
  int b = c.indexOf('|', a); return c.substring(a, b < 0 ? c.length() : b);
}
static String cardDigits() { String t = cardField(2), d; for (char ch : t) if (isdigit((unsigned char)ch)) d += ch; return d; }
static String cardTelNice() {                     // +33612345678 -> +33 6 12 34 56 78
  String d = cardDigits();
  if (d.startsWith("33") && d.length() == 11) return "+33 " + d.substring(2, 3) + " " + d.substring(3, 5) + " " + d.substring(5, 7) + " " + d.substring(7, 9) + " " + d.substring(9, 11);
  return cardField(2);
}
#define CAT_OFF (cardOn() ? 1 : 0)
#define N_CAT (N_STATIONS + CAT_OFF)
static void cardDraw() {
  String name = cardField(0), mail = cardField(1);
  textAt(EK_SMALL, 100, 1, name);
  const char *seg[3] = { L("both", "les deux"), "e-mail", "WhatsApp" };
  int w[3], tot = 0; for (int i = 0; i < 3; i++) { w[i] = textWidth2(EK_SMALL, seg[i]) + 12; tot += w[i]; }
  int x = 100 - tot / 2;
  for (int i = 0; i < 3; i++) {                     // the selector: the current choice inverted
    if (i == cardPg) rect(x, 17, w[i], 15, true);
    textLeft(EK_SMALL, x + 6, 17, seg[i], i != cardPg, w[i]);
    x += w[i];
  }
  String q;
  if (cardPg == 0) {
    int sp = name.lastIndexOf(' ');
    String given = sp > 0 ? name.substring(0, name.indexOf(' ')) : name, family = sp > 0 ? name.substring(name.indexOf(' ') + 1) : "";
    // v0.24.0: one name field (N), not two. The code loses about 22 characters, which is one size down for a usual card.
    q = "BEGIN:VCARD\nVERSION:3.0\nN:" + family + ";" + given + "\nTEL:+" + cardDigits() + "\nEMAIL:" + mail + "\nEND:VCARD";
  } else if (cardPg == 1) q = "mailto:" + mail;
  else q = "https://wa.me/" + cardDigits();
  String foot = cardPg == 0 ? String(L("saves the contact", "enregistre le contact")) : cardPg == 1 ? mail : cardTelNice();
  String foot2;                                     // too wide for one line: split an address at the @
  if (textWidth2(EK_SMALL, foot) > 196 && foot.indexOf('@') > 0) { int at = foot.indexOf('@'); foot2 = foot.substring(at); foot = foot.substring(0, at); }
  int box = foot2.length() ? 124 : 144;
  esp_qrcode_config_t cfg = { qrGrab, 10, ESP_QRCODE_ECC_LOW }; qrSize = 0;
  if (esp_qrcode_generate(&cfg, q.c_str()) == ESP_OK && qrSize) {
    int sc = max(1, min(5, box / (qrSize + 2))), side = qrSize * sc, x0 = 100 - side / 2, y0 = 36 + (box - side) / 2;
    for (int r = 0; r < qrSize; r++) for (int c = 0; c < qrSize; c++) if (qrM[r * qrSize + c]) rect(x0 + c * sc, y0 + r * sc, sc, sc, true);
  }
  if (foot2.length()) { textAt(EK_SMALL, 100, 164, foot); textAt(EK_SMALL, 100, 182, foot2); }
  else textAt(EK_SMALL, 100, 182, foot);
}
static void rdDraw() {
  epd->EPD_Clear();
  if (rv == RV_VCARD) { cardDraw(); show(); return; }
  if (rv == RV_CATLIST) {
    rdHeader("InvisibleChaos", String(N_STATIONS));
    drawList(N_CAT, rdCursor, [](int i, String &r) -> String {
      if (CAT_OFF && i == 0) { r = ""; return String(L("Business card", "Carte de visite")); }   // set apart by the rule below it
      const Station &st = STATIONS[i - CAT_OFF];
      r = st.tag[0] ? String(st.tag) : String(st.freq);
      return String(st.name);
    });
    if (CAT_OFF && rdCursor < 5) rect(4, BODY_Y + 25, 192, 2, true);     // the card, set apart from the stations
    rdFooter(L("\xE2\x80\xB9 \xE2\x80\xBA move  \xC2\xB7  middle shows", "\xE2\x80\xB9" " \xE2\x80\xBA" " d\xC3\xA9" "filer \xC2\xB7" " centre\xC2\xA0" ": voir"));
    show(); return;
  }
  if (rv == RV_CATQR) { catQr(rdCursor - CAT_OFF); show(); return; }
  if (rv == RV_LIBRARY) {
    rdHeader(L("editions", "\xC3\xA9" "ditions"), String(edN));
    drawList(edN, rdCursor, [](int i, String &r) -> String { r = edList[i].builtin ? "" : "SD"; return String(edList[i].name); });
    rdFooter(L("\xE2\x80\xB9 \xE2\x80\xBA move  \xC2\xB7  middle opens", "\xE2\x80\xB9" " \xE2\x80\xBA" " \xC2\xB7" " centre\xC2\xA0" ": ouvrir"));
  } else if (rv == RV_SPLASH) {
    rect(3, 3, 194, 2, true); rect(3, 195, 194, 2, true); rect(3, 3, 2, 194, true); rect(195, 3, 2, 194, true);
    rect(8, 8, 184, 1, true); rect(8, 191, 184, 1, true); rect(8, 8, 1, 184, true); rect(191, 8, 1, 184, true);
    textBlock(jgs(edRoot, "name", edList[edCur].name), 14, 28, 172, 96, true);
    String v = String("v") + jgs(edRoot, "version", "?") + "  \xC2\xB7  " + jgs(edRoot, "lang", "");
    textAt(EK_SMALL, 100, 128, v);
    int au = jget(edRoot, "authors");
    if (au >= 0 && jdoc[au].child >= 0) textAt(EK_SMALL, 100, 146, jstr(jdoc[au].child));
    textAt(EK_SMALL, 100, 170, L("tap to open \xE2\x80\xBA", "touchez pour ouvrir \xE2\x80\xBA"));
  } else if (rv == RV_SECTIONS) {
    rdHeader(jgs(edRoot, "name", edList[edCur].name), "");
    drawList(menuCount(), rdCursor, [](int i, String &r) -> String { return menuLabel(i, r); });
    rdFooter(L("\xE2\x80\xB9 \xE2\x80\xBA move  \xC2\xB7  middle opens", "\xE2\x80\xB9" " \xE2\x80\xBA" " \xC2\xB7" " centre\xC2\xA0" ": ouvrir"));
  } else if (rv == RV_DOC) {
    rdHeader(jgs(edRoot, "name", ""), pgN > 1 ? String(rdPage + 1) + "/" + String(pgN) : String(""));
    drawBodyPage(-1);
    rdFooter(rdPage + 1 < pgN ? L("more \xE2\x80\xBA", "suite \xE2\x80\xBA") : L("hold: back", "maintenir\xC2\xA0" ": retour"));
  } else if (rv == RV_CARD) {
    int ci = deck[dkPos];
    String where = dkKind == DK_SECTION ? String(secTitle[dkSec]) : dkKind == DK_REVIEW ? String(L("review", "r\xC3\xA9" "vision")) : String(L("daily prompt", "question du jour"));
    rdHeader(where, String(dkPos + 1) + "/" + String(deckN));
    drawBodyPage(ci);
    bool last = rdPage + 1 >= pgN;
    String f;
    if (isType(ci, "prompt")) f = !revealed ? L("tap middle to reveal", "centre\xC2\xA0" ": r\xC3\xA9" "v\xC3\xA9" "ler") : last ? L("\xE2\x80\xB9 again   \xC2\xB7   got it \xE2\x80\xBA", "\xE2\x80\xB9" " encore   \xC2\xB7" "   acquis \xE2\x80\xBA") : L("more \xE2\x80\xBA", "suite \xE2\x80\xBA");
    else if (isType(ci, "checklist") && rdPage == 0) f = L("tap an item to tick", "touchez pour cocher");
    else if (pgN > 1) f = String(rdPage + 1) + " / " + String(pgN);
    rdFooter(f);
  }
  show();
}

// ---- navigation
static void openCard(int pos, bool lastPage = false) {
  dkPos = pos; revealed = false; layoutCard(deck[dkPos]);
  rdPage = lastPage ? pgN - 1 : 0; rv = RV_CARD;
}
static void openDoc(const String &t, const String &b) { docTitle = t; docBody = b; layoutDoc(); rdPage = 0; rv = RV_DOC; }
static void startDeck(int kind, int sec) {
  dkKind = kind; dkSec = sec; dkPos = 0; buildDeck();
  if (!deckN) {
    if (kind == DK_REVIEW) openDoc(L("All done", "Termin\xC3\xA9"), L("Nothing is due today.\n\nPrompts come back on their own: after 1, 2, 4, 8 and 16 days as you get them right.", "Rien \xC3\xA0" " revoir aujourd\xE2\x80\x99" "hui.\n\nLes questions reviennent d\xE2\x80\x99" "elles-m\xC3\xAA" "mes\xC2\xA0" ": apr\xC3\xA8" "s 1, 2, 4, 8 et 16\xC2\xA0" "jours, \xC3\xA0" " mesure que vous les r\xC3\xA9" "ussissez."));
    else openDoc(L("Empty", "Vide"), L("No cards here yet.", "Pas encore de fiches ici."));
    return;
  }
  openCard(0);
}
static void rdSetEdition(int idx) {
  if (!loadEdition(idx)) { openDoc(L("Can't open", "Ouverture impossible"), String(L("This pack could not be read:\n", "Ce paquet n\xE2\x80\x99" "a pas pu \xC3\xAA" "tre lu\xC2\xA0" ":\n")) + edList[idx].slug + L("\n\nCheck edition.json and cards.json (see EDITIONS.md).", "\n\nV\xC3\xA9" "rifiez edition.json et cards.json (voir EDITIONS.md).")); return; }
  prefs.putString("rdEd", edList[edCur].slug);
  rv = RV_SPLASH; rdCursor = 0;
}
static void rdEnter() {                     // from the Editions tile
  rdOn = true; catMode = false; prefs.putBool("rdOn", true);
  scanEditions();
  if (edN > 1) { rv = RV_LIBRARY; rdCursor = 0; }
  else rdSetEdition(0);
  rdDraw();
}
static void catEnter() {                     // from the catalog tile
  rdOn = true; catMode = true; edCur = -1;
  prefs.putBool("rdOn", true); prefs.putString("rdEd", "@catalog");
  rv = RV_CATLIST; rdCursor = constrain(prefs.getInt("catLast", 0), 0, N_CAT - 1);
  rdDraw();
}
static void catalogFace() {                  // the tile: a small spectrum with the dial needle
  static const uint8_t H[13] = { 4, 7, 11, 16, 22, 27, 30, 27, 22, 16, 11, 7, 4 };
  for (int i = 0; i < 13; i++) rect(100 - 39 + i * 6, 82 - H[i], 4, H[i], true);
  rect(99, 38, 2, 50, true); disc(100, 36, 3);
  textAt(EK_MID, 100, 100, L("catalog", "catalogue"));
  textAt(EK_SMALL, 100, 132, String(N_STATIONS) + L(" stations", " stations"));
  textAt(EK_SMALL, 100, 164, L("tap to open", "touchez pour ouvrir"));
}
static void rdExit() {
  catMode = false;
  rdOn = false; prefs.putBool("rdOn", false);
  sdUnmount(); jfree();
  screenIdle();
}
static void rdUp() {
  if (rv == RV_CARD || rv == RV_DOC) { rv = RV_SECTIONS; }
  else if (rv == RV_SECTIONS) { rv = RV_SPLASH; }
  else if (rv == RV_SPLASH && (edScanned || (scanEditions(), true)) && edN > 1) { rv = RV_LIBRARY; rdCursor = edCur; }
  else { rdExit(); return; }
  rdDraw();
}
static void nextCard() {
  if (dkPos + 1 < deckN) openCard(dkPos + 1);
  else if (dkKind == DK_REVIEW) openDoc(L("Review done", "R\xC3\xA9" "vision termin\xC3\xA9" "e"), L("That was everything due today.", "C\xE2\x80\x99" "\xC3\xA9" "tait tout pour aujourd\xE2\x80\x99" "hui."));
  else if (dkKind == DK_POTD) openDoc(L("See you tomorrow", "\xC3\x80" " demain"), L("Today's prompt is done.\n\nHold to go to the sections.", "La question du jour est faite.\n\nMaintenez pour revenir aux sections."));
  else { rv = RV_SECTIONS; }
}
enum { EV_LEFT, EV_RIGHT, EV_MID, EV_HOLD };
static void rdEvent(int ev, int y) {
  if (rv == RV_VCARD) {                           // the business card: < > choose, hold back to the list
    if (ev == EV_LEFT) cardPg = (cardPg + 2) % 3;
    else if (ev == EV_RIGHT) cardPg = (cardPg + 1) % 3;
    else if (ev == EV_HOLD) rv = RV_CATLIST;
    else return;
    rdDraw(); return;
  }
  if (rv == RV_CATLIST || rv == RV_CATQR) {      // catalog: flip through stations either way
    if (ev == EV_LEFT) rdCursor = (rdCursor + N_CAT - 1) % N_CAT;
    else if (ev == EV_RIGHT) rdCursor = (rdCursor + 1) % N_CAT;
    else if (ev == EV_MID) {
      if (rv != RV_CATLIST) return;
      if (CAT_OFF && rdCursor == 0) { rv = RV_VCARD; cardPg = 0; rdDraw(); return; }
      rv = RV_CATQR;
    }
    else if (rv == RV_CATQR) rv = RV_CATLIST;      // hold: from a code back to the list
    else { rdExit(); return; }                     // hold on the list: back to the tiles
    if (rv == RV_CATQR && CAT_OFF && rdCursor == 0) rdCursor = ev == EV_LEFT ? N_CAT - 1 : 1;   // codes: stations only
    if (rv == RV_CATQR) prefs.putInt("catLast", rdCursor);
    rdDraw(); return;
  }
  if (rv == RV_LIBRARY) {
    if (ev == EV_LEFT) rdCursor = (rdCursor + edN - 1) % edN;
    else if (ev == EV_RIGHT) rdCursor = (rdCursor + 1) % edN;
    else if (ev == EV_MID) rdSetEdition(rdCursor);
    else { rdExit(); return; }
  } else if (rv == RV_SPLASH) {
    if (ev == EV_MID || ev == EV_RIGHT) { rv = RV_SECTIONS; rdCursor = 0; }
    else { rdUp(); return; }
  } else if (rv == RV_SECTIONS) {
    int n = menuCount();
    if (ev == EV_LEFT) rdCursor = (rdCursor + n - 1) % n;
    else if (ev == EV_RIGHT) rdCursor = (rdCursor + 1) % n;
    else if (ev == EV_HOLD) { rdUp(); return; }
    else if (rdCursor < nSecs) startDeck(DK_SECTION, rdCursor);
    else if (rdCursor == nSecs) startDeck(DK_REVIEW, 0);
    else if (rdCursor == nSecs + 1) prefs.putBool("potd", !potdOn());
    else if (rdCursor == nSecs + 2) { smallText = !smallText; prefs.putBool("edSmall", smallText); }
    else if (rdCursor == nSecs + 3 && langCount(edList[edCur].slug) > 1) {   // next language, same place, same progress
      const char *nx = nextLangSlug(edList[edCur].slug);
      prefs.putString("edLang", langOf(nx));
      int k = edCur; strlcpy(edList[k].slug, nx, sizeof edList[k].slug);
      if (!loadEdition(k)) { rdSetEdition(k); return; }
      prefs.putString("rdEd", edList[k].slug);
    }
    else {
      String b = String("Version ") + jgs(edRoot, "version", "?") + "  \xC2\xB7  " + jgs(edRoot, "lang", "") + "\n";
      int au = jget(edRoot, "authors"); for (int n2 = au >= 0 ? jdoc[au].child : -1; n2 >= 0; n2 = jdoc[n2].next) b += String(jstr(n2)) + "\n";
      b += String(L("License: ", "Licence\xC2\xA0" ": ")) + jgs(edRoot, "license", "?") + "\n\n**Sources**\n";
      int so = jget(edRoot, "sources"); for (int n2 = so >= 0 ? jdoc[so].child : -1; n2 >= 0; n2 = jdoc[n2].next) b += String(jstr(n2)) + "\n";
      openDoc(L("About", "\xC3\x80" " propos"), b);
    }
  } else if (rv == RV_DOC) {
    if (ev == EV_RIGHT && rdPage + 1 < pgN) rdPage++;
    else if (ev == EV_LEFT && rdPage > 0) rdPage--;
    else if (ev == EV_HOLD || ev == EV_MID || ev == EV_LEFT) { rv = RV_SECTIONS; }
  } else if (rv == RV_CARD) {
    int ci = deck[dkPos]; bool last = rdPage + 1 >= pgN;
    bool prompt = isType(ci, "prompt");
    if (ev == EV_HOLD) { rv = RV_SECTIONS; }
    else if (ev == EV_MID) {
      if (prompt && !revealed) {
        revealed = true; layoutCard(ci);
        int ans = -1;                       // jump to the page where the answer starts
        for (int i = 0; i < rlN; i++) if (rl[i].kind == K_RULE) { ans = min(i + 1, rlN - 1); break; }
        for (int p = 0; ans >= 0 && p < pgN; p++) if (pgStart[p] <= ans && ans < pageEnd(p)) rdPage = p;
      } else if (isType(ci, "checklist")) {
        for (int k = 0; k < 32; k++) if (itemTop[k] >= 0 && y >= itemTop[k] - 2 && y < itemBot[k] + 2) { edTick[ci] ^= 1UL << k; saveState(); break; }
      } else return;                        // nothing to do on this card: no redraw
    } else if (ev == EV_RIGHT) {
      if (!last) rdPage++;
      else { if (prompt && revealed) rate(ci, true); nextCard(); }
    } else if (ev == EV_LEFT) {
      if (prompt && revealed && last) { rate(ci, false); nextCard(); }
      else if (rdPage > 0) rdPage--;
      else if (dkPos > 0) openCard(dkPos - 1, true);
      else { rv = RV_SECTIONS; }
    }
  }
  rdDraw();
}
// read one gesture while awake and turn it into a reader event
static void rdTouch() {
  int x0 = 0, y0 = 0, x = 0, y = 0;
  touchXY(x0, y0); x = x0; y = y0;
  uint32_t t0 = millis(); bool moved = false, down = true, hold = false;
  while (down) {
    int nx, ny; down = touchXY(nx, ny);
    if (down) { x = nx; y = ny; }
    if (abs(x - x0) > SWIPE_PX || abs(y - y0) > SWIPE_PX) moved = true;
    if (!moved && down && millis() - t0 >= RD_HOLD_MS) { hold = true; break; }
    delay(12);
  }
  waitRelease();
  if (hold) { rdEvent(EV_HOLD, y0); return; }
  if (moved) { int dx = x - x0, dy = y - y0; int d = abs(dx) >= abs(dy) ? dx : dy; bool next = d < 0; if (SWIPE_FLIP) next = !next; rdEvent(next ? EV_RIGHT : EV_LEFT, y0); return; }
  int tx = TOUCH_FLIP_X ? 199 - x0 : x0;
  rdEvent(tx < 67 ? EV_LEFT : tx > 133 ? EV_RIGHT : EV_MID, y0);
}
// the chip usually keeps the last point after the finger lifts (event flag 01 = lift-up)
static bool lastLift(int &x, int &y) {
  uint8_t r[5];
  if (i2c_read_buff(touch_handle, 0x02, r, 5) != 0) return false;
  if ((r[0] & 0x0F) != 0 || (r[1] >> 6) != 1) return false;
  x = ((r[1] & 0x0F) << 8) | r[2]; y = ((r[3] & 0x0F) << 8) | r[4];
  return x < 200 && y < 200 && (x || y);
}
static void rdPrime() {                      // rebuild hit areas (checkboxes) without drawing
  if (rv != RV_CARD) return;
  epd->EPD_Clear(); drawBodyPage(deck[dkPos]);
}
// sleep / wake bookkeeping
static void rdSaveRtc() {
  rtcRdOn = rdOn; if (!rdOn) return;
  strlcpy(rtcRdSlug, catMode ? "@catalog" : edCur >= 0 ? edList[edCur].slug : "edition-zero", sizeof rtcRdSlug);
  rtcRdView = rv == RV_DOC ? RV_SECTIONS : rv; rtcRdKind = dkKind; rtcRdRev = revealed;
  rtcRdCursor = rdCursor; rtcRdSec = dkSec; rtcRdPos = dkPos; rtcRdPage = rdPage;
}
static bool rdRestore(const char *slug) {     // reopen after a wake or a cold start
  rdOn = true;
  if (!strcmp(slug, "@catalog")) { catMode = true; edCur = -1; return true; }   // the catalog needs no pack
  catMode = false;
  if (builtinIdx(slug) >= 0) { builtinOnly(); edScanned = false; }   // built in: no need to wake the card
  else scanEditions();
  int i = edFind(slug); if (i < 0) i = 0;
  if (!loadEdition(i)) { rdSetEdition(i); return false; }
  return true;
}
static bool potdWanted() {                   // a new day and the daily prompt is on
  if (!potdOn() || edCur < 0) return false;
  if (timeValid()) { time_t e = time(NULL); struct tm t; localtime_r(&e, &t); if (t.tm_hour < 7) return false; }
  return rtcPotdDay != today();
}
static void showPotd() {
  rtcPotdCard = pickPotd(); rtcPotdDay = today(); prefs.putInt("potdDay", rtcPotdDay);
  if (rtcPotdCard < 0) { rv = RV_SECTIONS; return; }
  dkKind = DK_POTD; dkPos = 0; buildDeck(); openCard(0);
}
static uint64_t potdTimerUs() {              // next 07:00, if the daily prompt is on
  if (!rdOn || !potdOn() || !timeValid()) return 0;
  time_t e = time(NULL); struct tm t; localtime_r(&e, &t);
  int32_t secs = (7 - t.tm_hour) * 3600 - t.tm_min * 60 - t.tm_sec;
  if (secs <= 0) secs += 86400;
  if (rtcPotdDay != today() && t.tm_hour >= 7) secs = 60;
  return (uint64_t)secs * 1000000ULL;
}

#if BATLOG
// ---- log export: the last 7 days as a few QR pages, to paste into a chat for calibration.
// Payload (QR alphanumeric mode): "EK1 P<page>/<pages> T<epoch> <mv> <mv> ... T<epoch> <mv> ..."
// A T token starts every page and stamps any sample that does not follow the one before by 30 minutes (20 to 40).
#define BX_PER_PAGE 36
#define BX_MAXPG 60
#define BX_BUDGET 300                           // characters per page: QR version 10 at level M holds 311 of this alphabet
static bool bxOn = false; static int bxPage = 0, bxPages = 0;
static String bxText;                                      // the current page's payload
static void bxBuild(int page) {                 // pages are cut by size (v0.23.2), so walk from the start each time
  int n; BatSample *a = loadLog(n);
  uint32_t from = time(NULL) - 7UL * 86400; int s0 = 0;
  while (s0 < n && a[s0].t < from) s0++;
  int pg = 0, cnt = 0; String cur; bxText = "";
  for (int i = s0; i <= n; i++) {
    String tok;
    if (i < n) tok = (logGap(a, i, s0) ? String(" T") + String(a[i].t) : String("")) + " " + String(a[i].mv);
    if (i == n || (cnt && (cnt >= BX_PER_PAGE || cur.length() + tok.length() > BX_BUDGET - 12))) {   // close this page
      if (pg == page) bxText = cur;
      pg++; if (i == n || pg >= BX_MAXPG) break;
      cur = ""; cnt = 0; tok = String(" T") + String(a[i].t) + " " + String(a[i].mv);                // a page always starts with T
    }
    cur += tok; cnt++;
  }
  bxPages = max(1, pg);
  bxText = "EK1 P" + String(page + 1) + "/" + String(bxPages) + bxText;
  if (a) free(a);
}
static void bxDraw() {
  epd->EPD_Clear();
  textAt(EK_SMALL, 100, 1, "battery log  " + String(bxPage + 1) + "/" + String(bxPages));
  esp_qrcode_config_t cfg = { qrGrab, 10, ESP_QRCODE_ECC_MED };
  qrSize = 0;
  if (esp_qrcode_generate(&cfg, bxText.c_str()) == ESP_OK && qrSize) {
    int sc = min(5, 160 / (qrSize + 2)), side = qrSize * sc, x0 = 100 - side / 2, y0 = 20 + (160 - side) / 2;
    for (int r = 0; r < qrSize; r++) for (int c = 0; c < qrSize; c++) if (qrM[r * qrSize + c]) rect(x0 + c * sc, y0 + r * sc, sc, sc, true);
  } else textBlock("page too long for a code", 8, 60, 184, 80, false);
  textAt(EK_SMALL, 100, 182, "\xE2\x80\xB9 \xE2\x80\xBA pages  \xC2\xB7  hold: back");
  show();
}
static void bxEnter() { bxOn = true; bxPage = 0; bxBuild(0); bxDraw(); }
static void bxExit() { bxOn = false; bxText = ""; screenIdle(); }
static void bxTouch() {
  int x0 = 0, y0 = 0; touchXY(x0, y0);
  uint32_t t0 = millis(); bool hold = false;
  while (touched()) { if (millis() - t0 >= 600) { hold = true; break; } delay(12); }
  waitRelease();
  if (hold) { bxExit(); return; }
  int tx = TOUCH_FLIP_X ? 199 - x0 : x0;
  int np = tx < 67 ? bxPage - 1 : bxPage + 1;
  if (np < 0 || np >= bxPages) return;
  bxPage = np; bxBuild(bxPage); bxDraw();
}
#endif


#if CLIMATE
// ---- climate log export: the last 7 days (room temp, room humidity, outdoor temp) as QR pages.
// Payload "EK2 P<page>/<pages> T<epoch> <blob> [T<epoch> <blob>...]": a T token starts each run of
// 30-minute samples; each sample is 5 base-44 characters (QR alphanumeric set without the space) of
//   v = (a * 102 + b) * 122 + c,  a = tin10 + 400 (0..1200, 1201 = none), b = rh (0..100, 101 = none),
//                                 c = tout + 60  (0..120,  121 = none)
static const char CX_AL[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$%*+-./:";
#define CX_MAXPG 40
#define CX_BUDGET 276                           // characters per page incl. header: QR version 8 (279 max), drawn at 3 px
static bool cxOn = false; static int cxPage = 0, cxPages = 0; static String cxText;
static void cxEnc(const BatSample &s, char *o) {
  uint32_t a = s.tin10 == INT16_MIN ? 1201 : (uint32_t)(constrain((int)s.tin10, -400, 800) + 400);
  uint32_t b = s.rh == 255 ? 101 : min((int)s.rh, 100);
  uint32_t c = s.tout == -128 ? 121 : (uint32_t)(constrain((int)s.tout, -60, 60) + 60);
  uint32_t v = (a * 102 + b) * 122 + c;
  for (int i = 4; i >= 0; i--) { o[i] = CX_AL[v % 44]; v /= 44; }
  o[5] = 0;
}
static void cxBuild(int page) {                  // pages are cut by size, so walk from the start each time
  int n; BatSample *a = loadLog(n);
  uint32_t from = time(NULL) - 7UL * 86400; int s0 = 0;
  while (s0 < n && a[s0].t < from) s0++;
  int pg = 0; String cur; cxText = "";
  for (int i = s0; i <= n; i++) {
    String tok;
    if (i < n) {
      bool gap = logGap(a, i, s0);
      char e[6]; cxEnc(a[i], e);
      tok = gap ? String(" T") + String(a[i].t) + " " + e : String(e);
    }
    if (i == n || (cur.length() && cur.length() + tok.length() > CX_BUDGET - 12)) {   // close this page
      if (pg == page) cxText = cur;
      pg++; if (i == n || pg >= CX_MAXPG) break;
      cur = ""; char e[6]; cxEnc(a[i], e); tok = String(" T") + String(a[i].t) + " " + e;   // a page always starts with T
    }
    cur += tok;
  }
  cxPages = max(1, pg);
  cxText = "EK2 P" + String(page + 1) + "/" + String(cxPages) + cxText;
  if (a) free(a);
}
static void cxDraw() {
  epd->EPD_Clear();
  textAt(EK_SMALL, 100, 1, String(L("climate log  ", "journal climat  ")) + String(cxPage + 1) + "/" + String(cxPages));
  esp_qrcode_config_t cfg = { qrGrab, 10, ESP_QRCODE_ECC_LOW }; qrSize = 0;
  if (esp_qrcode_generate(&cfg, cxText.c_str()) == ESP_OK && qrSize) {
    int sc = min(5, 160 / (qrSize + 2)), side = qrSize * sc, x0 = 100 - side / 2, y0 = 20 + (160 - side) / 2;
    for (int r = 0; r < qrSize; r++) for (int c = 0; c < qrSize; c++) if (qrM[r * qrSize + c]) rect(x0 + c * sc, y0 + r * sc, sc, sc, true);
  }
  textAt(EK_SMALL, 100, 182, L("\xE2\x80\xB9 \xE2\x80\xBA pages  \xC2\xB7  hold: back", "\xE2\x80\xB9 \xE2\x80\xBA pages \xC2\xB7 maintenir"));
  show();
}
static void cxEnter() { cxOn = true; cxPage = 0; cxBuild(0); cxDraw(); }
static void cxExit() { cxOn = false; cxText = ""; climOn = false; screenIdle(); }
static void cxTouch() {
  int x0 = 0, y0 = 0; touchXY(x0, y0);
  uint32_t t0 = millis(); bool hold = false;
  while (touched()) { if (millis() - t0 >= 600) { hold = true; break; } delay(12); }
  waitRelease();
  if (hold) { cxExit(); return; }
  int tx = TOUCH_FLIP_X ? 199 - x0 : x0;
  int np = tx < 67 ? cxPage - 1 : cxPage + 1;
  if (np < 0 || np >= cxPages) return;
  cxPage = np; cxBuild(cxPage); cxDraw();
}
#endif

// ================================================================ ALARM & TIMER (clock deep dive)
// Middle tap on the clock: Alarm; swipe: Timer; hold: back. Rings from sleep.
#define RING_MAX_S    600                   // an unanswered ring stops after 10 minutes
#define ESCALATE_S    180                   // gentle ringers become the beacon after 3 minutes
#define SNOOZE_S      540                   // 9 minutes
#define FIP_LEAD_S    60                    // wake early: connect, let any pre-roll pass muted
#define FIP_URL       "https://icecast.radiofrance.fr/fip-midfi.mp3"          // fallback: MP3, may play a pre-roll ad
#define FIP_HLS       "https://stream.radiofrance.fr/fip/fip_hifi.m3u8"     // first choice: segments, no pre-roll
static const char *TONE_NAME_FR[] = { "FIP", "carillon", "balise" };
static const char *TONE_NAME_EN[] = { "FIP", "chime", "beacon" };
static const char *REP_NAME_FR[]  = { "une fois", "lun-ven", "quotidien" };
static const char *REP_NAME_EN[]  = { "once", "weekdays", "daily" };
#define TONE_NAME (uiFr ? TONE_NAME_FR : TONE_NAME_EN)   // v0.26.0: chosen when used
#define REP_NAME  (uiFr ? REP_NAME_FR : REP_NAME_EN)
enum { EV_NONE, EV_ALARM, EV_SNOOZE, EV_TIMER };
RTC_DATA_ATTR static uint32_t rtcLastAlarmAt;

static uint32_t alarmAt() {                 // next alarm, or 0; a just-missed one (2 min) still counts
  if (!alOn || !timeValid()) return 0;
  time_t now = time(NULL);
  for (int d = 0; d < 8; d++) {
    time_t base = now + d * 86400; struct tm t; localtime_r(&base, &t);
    t.tm_hour = alH; t.tm_min = alM; t.tm_sec = 0; t.tm_isdst = -1;
    time_t e = mktime(&t);
    if (e <= (time_t)rtcLastAlarmAt || e < now - 120) continue;
    if (alRep == 1 && (t.tm_wday == 0 || t.tm_wday == 6)) continue;
    return (uint32_t)e;
  }
  return 0;
}
static uint32_t nextEvent(int &kind) {
  uint32_t best = 0; kind = EV_NONE;
  uint32_t a = alarmAt();
  if (a) { best = a; kind = EV_ALARM; }
  if (rtcSnoozeAt && (!best || rtcSnoozeAt < best)) { best = rtcSnoozeAt; kind = EV_SNOOZE; }
  if (rtcTimerEnd && (!best || rtcTimerEnd < best)) { best = rtcTimerEnd; kind = EV_TIMER; }
  return best;
}
static int leadFor(int kind) {                // FIP needs time to connect; a timer only waits briefly
  int t = kind == EV_TIMER ? tmTone : alTone;
  return t != TONE_FIP ? 2 : kind == EV_TIMER ? 20 : FIP_LEAD_S;
}
static uint64_t eventWakeUs() {             // long sleeps are split: re-sync from the clock chip first
  int k; uint32_t ev = nextEvent(k); if (!ev) return 0;
  int64_t target = (int64_t)ev - leadFor(k) - (int64_t)time(NULL);
  if (target > 900) target -= 300;          // wake 5 min early, correct any drift, sleep the rest
  return (uint64_t)max((int64_t)1, target) * 1000000ULL;
}

static void beep(int hz, int ms);
// ---- sound: the codec runs at 16 kHz stereo; FIP reopens it at the stream's rate
static esp_codec_dev_handle_t pbDev() { return get_playback_handle(); }
static int pbRate = EK_SAMPLE_RATE;
static void pbRestore() {                   // normal volume after a ring (the codec stays at 16 kHz)
  esp_codec_dev_set_out_vol(pbDev(), 70.0);
  if (pbRate == EK_SAMPLE_RATE) return;
  esp_codec_dev_sample_info_t fs = {}; fs.sample_rate = EK_SAMPLE_RATE; fs.channel = 2; fs.bits_per_sample = 16;
  esp_codec_dev_close(pbDev()); esp_codec_dev_open(pbDev(), &fs);
  esp_codec_dev_handle_t rc = get_record_handle();
  esp_codec_dev_close(rc); esp_codec_dev_open(rc, &fs);
  pbRate = EK_SAMPLE_RATE;
}
// one 16 kHz sample in -1..1 for a ringer (pitched where a small speaker is loudest)
static float toneSample(int tone, Synth &s) {
  float t = s.n / (float)EK_SAMPLE_RATE; s.n++;
  if (tone == TONE_CHIME) {                  // three bright rising notes every 2.4 s
    float c = fmodf(t, 2.4f), v = 0; static const float F[3] = { 2093.0f, 2637.0f, 3136.0f };
    for (int i = 0; i < 3; i++) {
      float u = c - i * 0.35f;
      if (u >= 0) { float ph = 2 * PI * F[i] * u; v += (sinf(ph) + 0.3f * sinf(3 * ph)) * expf(-u / 0.35f); }
    }
    return v * 0.75f;
  }
  // beacon: two 2.4 kHz beeps every second
  float c = fmodf(t, 1.0f); bool on = c < 0.15f || (c > 0.25f && c < 0.40f);
  return on ? sinf(2 * PI * 2400 * t) * 0.95f : 0;
}
static void playChunk(int tone, Synth &s, float gain, int frames) {
  static int16_t b[512 * 2];
  for (int i = 0; i < frames; i++) { int16_t v = (int16_t)(constrain(toneSample(tone, s) * gain, -1.0f, 1.0f) * 31000); b[2 * i] = v; b[2 * i + 1] = v; }
  esp_codec_dev_write(pbDev(), b, frames * 4);
}
static void preview(int tone) {              // two seconds of a ringer, at a moderate level
  if (tone == TONE_FIP) { beep(1320, 70); return; }
  Synth s; for (int i = 0; i < EK_SAMPLE_RATE * 2 / 256; i++) playChunk(tone, s, 0.5f, 256);
}

// ---- FIP over HTTPS, decoded with the Helix MP3 decoder built into the ESP32 core.
// A network task fills a 48 KB buffer; the ring loop decodes, filters and resamples to 16 kHz.
#include "freertos/stream_buffer.h"
#define RS_TAPS   96                         // windowed-sinc low-pass + resampler: steep enough for music
#define RS_PHASES 128
// Strips HTTP chunk framing and ICY metadata so the decoder only ever sees MP3 bytes.
// Works in place on each network read (output never longer than input), across read boundaries.
struct StreamClean {
  bool chunked = false; int metaInt = 0;
  int cstate = 0; uint32_t hexVal = 0; bool sawHex = false, inExt = false; int chunkLeft = 0;   // chunked
  int audioLeft = 0, metaLeft = -1;                                                           // icy
  void begin(bool ch, int mi) { chunked = ch; metaInt = mi; cstate = 0; hexVal = 0; sawHex = inExt = false; chunkLeft = 0; audioLeft = mi; metaLeft = -1; }
  int dechunk(uint8_t *b, int n) {
    int w = 0;
    for (int r = 0; r < n; ) {
      if (cstate == 1) {                         // chunk data
        int k = min(chunkLeft, n - r); memmove(b + w, b + r, k); w += k; r += k; chunkLeft -= k;
        if (!chunkLeft) cstate = 0;
        continue;
      }
      if (cstate == 3) break;                    // last chunk seen: the stream is over
      uint8_t c = b[r++];                        // size line: hex digits, optional ";ext", CRLF
      if (c == '\n') {
        if (sawHex) { chunkLeft = hexVal; cstate = chunkLeft ? 1 : 3; }
        hexVal = 0; sawHex = false; inExt = false;
      } else if (c == ';') inExt = true;
      else if (!inExt) {
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d >= 0) { hexVal = hexVal * 16 + d; sawHex = true; }
      }
    }
    return w;
  }
  int deicy(uint8_t *b, int n) {
    int w = 0;
    for (int r = 0; r < n; ) {
      if (audioLeft > 0) { int k = min(audioLeft, n - r); memmove(b + w, b + r, k); w += k; r += k; audioLeft -= k; continue; }
      if (metaLeft < 0) { metaLeft = b[r++] * 16; if (!metaLeft) { audioLeft = metaInt; metaLeft = -1; } continue; }
      int k = min(metaLeft, n - r); r += k; metaLeft -= k;
      if (!metaLeft) { audioLeft = metaInt; metaLeft = -1; }
    }
    return w;
  }
  int run(uint8_t *b, int n) { if (chunked) n = dechunk(b, n); if (metaInt > 0) n = deicy(b, n); return n; }
};

static int radioTest = 0;                   // 0 music, 1 test A (tone, Wi-Fi off), 2 test B (tone while FIP streams)
static double testPh = 0;
static void testTone(float *x, int frames, int rate) {          // 440 Hz, steady
  for (int i = 0; i < frames; i++) { x[i] = 9000.0f * sinf((float)testPh); testPh += 2 * PI * 440.0 / rate; if (testPh > 2 * PI) testPh -= 2 * PI; }
}
// MPEG-TS -> ADTS AAC. Feeds on any byte chunks (network reads), keeps partial packets between calls.
// PAT -> PMT -> the audio PID (ADTS AAC, stream type 0x0F); PES headers stripped; ADTS frames out.
struct TsDemux {
  uint8_t pkt[188]; int have = 0;
  int pmtPid = -1, audPid = -1;
  bool inPes = false; int pesSkip = 0;          // bytes of PES header still to drop
  uint8_t pesHdr[16]; int pesHdrN = 0;          // a PES header split across packets
  void reset() { have = 0; pmtPid = audPid = -1; inPes = false; pesSkip = 0; pesHdrN = 0; }
  // push bytes; emits audio bytes through out(); returns audio bytes emitted
  template <class F> int push(const uint8_t *b, int n, F out) {
    int emitted = 0;
    for (int i = 0; i < n; ) {
      if (have == 0 && b[i] != 0x47) { i++; continue; }          // resync on the sync byte
      int k = 188 - have; if (k > n - i) k = n - i;
      memcpy(pkt + have, b + i, k); have += k; i += k;
      if (have == 188) { emitted += packet(out); have = 0; }
    }
    return emitted;
  }
  template <class F> int packet(F out) {
    const uint8_t *p = pkt;
    if (p[0] != 0x47) return 0;
    bool pusi = p[1] & 0x40; int pid = ((p[1] & 0x1F) << 8) | p[2]; int afc = (p[3] >> 4) & 3;
    int off = 4;
    if (afc & 2) off += 1 + p[4];
    if (!(afc & 1) || off >= 188) return 0;
    const uint8_t *pay = p + off; int len = 188 - off;
    if (pid == 0 && pusi) {                                        // PAT: first real program -> PMT pid
      int ptr = pay[0]; const uint8_t *t = pay + 1 + ptr; int sl = ((t[1] & 0x0F) << 8) | t[2];
      for (int j = 8; j + 4 <= 3 + sl - 4; j += 4) {
        int prog = (t[j] << 8) | t[j + 1]; int pp = ((t[j + 2] & 0x1F) << 8) | t[j + 3];
        if (prog != 0) { pmtPid = pp; break; }
      }
      return 0;
    }
    if (pid == pmtPid && pusi) {                                   // PMT: the AAC stream
      int ptr = pay[0]; const uint8_t *t = pay + 1 + ptr; int sl = ((t[1] & 0x0F) << 8) | t[2];
      int pil = ((t[10] & 0x0F) << 8) | t[11]; int j = 12 + pil, end = 3 + sl - 4;
      int first = -1;
      while (j + 5 <= end) {
        int st = t[j], ep = ((t[j + 1] & 0x1F) << 8) | t[j + 2], il = ((t[j + 3] & 0x0F) << 8) | t[j + 4];
        if (st == 0x0F) { audPid = ep; break; }
        if (first < 0 && (st == 0x03 || st == 0x04)) first = ep;    // (MPEG audio: noted, not decoded here)
        j += 5 + il;
      }
      return 0;
    }
    if (pid != audPid || audPid < 0) return 0;
    if (pusi) { inPes = true; pesHdrN = 0; pesSkip = -1; }
    if (!inPes) return 0;
    int i = 0;
    if (pesSkip < 0) {                                             // collect 9 bytes to learn the header size
      while (pesHdrN < 9 && i < len) pesHdr[pesHdrN++] = pay[i++];
      if (pesHdrN < 9) return 0;
      if (pesHdr[0] != 0 || pesHdr[1] != 0 || pesHdr[2] != 1) { inPes = false; return 0; }
      pesSkip = pesHdr[8];
    }
    int d = pesSkip < len - i ? pesSkip : len - i; i += d; pesSkip -= d;
    if (i >= len) return 0;
    out(pay + i, len - i);
    return len - i;
  }
};

struct FipStream {
  NetworkClientSecure cli; HMP3Decoder dec = NULL;
  HAACDecoder adec = NULL; bool aac = false;   // HLS engine: AAC in MPEG-TS segments
  TsDemux ts; String hlsUrl; uint32_t hlsSegs = 0;
  StreamBufferHandle_t sb = NULL; TaskHandle_t task = NULL;
  StreamBufferHandle_t pcmQ = NULL; TaskHandle_t outT = NULL;     // 1 s of 16 kHz stereo, fed to the codec by its own task
  StaticStreamBuffer_t sbCtl, pcmCtl; uint8_t *sbMem = NULL, *pcmMem = NULL;
  volatile bool lost = false, quit = false;
  uint8_t in[4096]; int left = 0; uint8_t *ptr = in; int16_t pcm[4096];      // AAC with SBR: up to 2048 x 2
  float hist[RS_TAPS + 2048]; float rsPos = 0; int rsRate = 0; bool needMore = false;
  StreamClean clean;                          // strips chunk framing / ICY metadata before the decoder
  uint32_t nOk = 0, nBad = 0, nJunk = 0; int kbps = 0, srate = 0;   // on-air readout
  // speaker stage at 16 kHz: 2nd-order Butterworth high-pass at 200 Hz (RBJ), boost, peak limiter
  float hb0 = 0, hb1 = 0, hb2 = 0, ha1 = 0, ha2 = 0, hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0;
  float boost = 1.0f, limEnv = 0;
  void hpInit() {
    float w = 2 * PI * 200.0f / EK_SAMPLE_RATE, c = cosf(w), al = sinf(w) / (2 * 0.7071f), a0 = 1 + al;
    hb0 = (1 + c) / 2 / a0; hb1 = -(1 + c) / a0; hb2 = (1 + c) / 2 / a0; ha1 = -2 * c / a0; ha2 = (1 - al) / a0;
  }
  inline float speaker(float x, float gain) {
    float y = hb0 * x + hb1 * hx1 + hb2 * hx2 - ha1 * hy1 - ha2 * hy2;
    hx2 = hx1; hx1 = x; hy2 = hy1; hy1 = y;
    y *= gain * boost;
    const float T = 29000.0f;                 // limiter: instant on a peak, ~150 ms recovery
    float a = fabsf(y); limEnv = a > limEnv ? a : limEnv * 0.99958f;
    if (limEnv > T) y *= T / limEnv;
    return y;
  }
  float (*coef)[RS_TAPS] = NULL;            // RS_PHASES x RS_TAPS, in fast internal RAM
  static void outTask(void *arg) {           // cushion -> speaker; never waits on network, touch or paper
    FipStream *f = (FipStream *)arg; static int16_t b[1024];
    while (!f->quit) {
      size_t n = xStreamBufferReceive(f->pcmQ, b, sizeof b, pdMS_TO_TICKS(100));
      if (n >= 4) esp_codec_dev_write(pbDev(), b, n & ~3u);
    }
    f->outT = NULL; vTaskDelete(NULL);
  }
  static void netTask(void *arg) {           // network -> buffer, independent of the speaker
    FipStream *f = (FipStream *)arg; static uint8_t tmp[1460]; uint32_t lastData = millis();
    while (!f->quit) {
      int n = f->cli.available() ? f->cli.read(tmp, sizeof tmp) : 0;
      if (n > 0) n = f->clean.run(tmp, n);     // only MP3 bytes go on: framing and metadata stripped
      if (n > 0) {                             // every byte goes in: a missing byte is a garble
        lastData = millis(); int sent = 0;
        while (sent < n && !f->quit) sent += xStreamBufferSend(f->sb, tmp + sent, n - sent, pdMS_TO_TICKS(200));
      }
      else {
        if (millis() - lastData > 5000 || !f->cli.connected()) { f->lost = true; break; }
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    f->task = NULL; vTaskDelete(NULL);
  }
  // GET over HTTPS (HTTP/1.0, so no chunking), following redirects; the body is streamed to onData.
  template <class F> static bool httpsGet(String url, F onData, volatile bool &quit, String *finalUrl = nullptr) {
    for (int hop = 0; hop < 3; hop++) {
      if (!url.startsWith("https://")) return false;
      int sl = url.indexOf('/', 8);
      String host = url.substring(8, sl < 0 ? url.length() : sl), path = sl < 0 ? "/" : url.substring(sl);
      NetworkClientSecure c; c.setInsecure(); c.setTimeout(8000);
      if (!c.connect(host.c_str(), 443)) return false;
      c.print("GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: Einkster\r\nConnection: close\r\n\r\n");
      String status = c.readStringUntil('\n'); String loc; long clen = -1;
      while (c.connected() || c.available()) {
        String h = c.readStringUntil('\n'); h.trim(); if (!h.length()) break;
        String l = h; l.toLowerCase();
        if (l.startsWith("location:")) { loc = h.substring(9); loc.trim(); }
        if (l.startsWith("content-length:")) clen = l.substring(15).toInt();
      }
      if ((status.indexOf(" 301") > 0 || status.indexOf(" 302") > 0 || status.indexOf(" 307") > 0) && loc.length()) {
        c.stop(); url = loc.startsWith("/") ? "https://" + host + loc : loc; continue;
      }
      if (status.indexOf(" 200") < 0) { c.stop(); return false; }
      if (finalUrl) *finalUrl = url;
      static uint8_t buf[1460]; long got = 0; uint32_t last = millis();
      while (!quit && (clen < 0 || got < clen)) {
        int n = c.available() ? c.read(buf, sizeof buf) : 0;
        if (n > 0) { onData(buf, n); got += n; last = millis(); }
        else { if (!c.connected() || millis() - last > 8000) break; vTaskDelay(pdMS_TO_TICKS(3)); }
      }
      c.stop(); return clen < 0 || got >= clen;
    }
    return false;
  }
  static String resolveUrl(const String &base, const String &u) {
    if (u.startsWith("http")) return u;
    int sl = base.indexOf('/', 8);
    if (u.startsWith("/")) return base.substring(0, sl < 0 ? base.length() : sl) + u;
    int q = base.indexOf('?'); String b = q >= 0 ? base.substring(0, q) : base;
    return b.substring(0, b.lastIndexOf('/') + 1) + u;
  }
  void nap(int ms) { for (int t = 0; t < ms && !quit; t += 50) vTaskDelay(pdMS_TO_TICKS(50)); }   // a sleep that hears "stop"
  void sendAll(const uint8_t *p, int n) {    // every byte goes in (the decoder waits, never skips)
    int sent = 0; while (sent < n && !quit) sent += xStreamBufferSend(sb, p + sent, n - sent, pdMS_TO_TICKS(200));
  }
  // playlist -> new segments -> TS demux -> AAC bytes into the buffer; 3 segments behind the live edge
  static void hlsTask(void *arg) {
    FipStream *f = (FipStream *)arg; String pl = f->hlsUrl; long long lastSeq = -1; int fails = 0;
    while (!f->quit) {
      String text, real;
      bool ok = httpsGet(pl, [&](const uint8_t *b, int n) { if (text.length() < 16000) text.concat((const char *)b, n); }, f->quit, &real);
      if (!ok) { if (++fails >= 4) { f->lost = true; break; } f->nap(1500); continue; }
      fails = 0;
      long long seq0 = 0; int target = 4, cnt = 0; String uris[24]; String variant; long bw = -1, bestBw = -1;
      for (int a = 0; a < (int)text.length(); ) {
        int e = text.indexOf('\n', a); if (e < 0) e = text.length();
        String ln = text.substring(a, e); ln.trim(); a = e + 1;
        if (!ln.length()) continue;
        if (ln.startsWith("#EXT-X-MEDIA-SEQUENCE:")) seq0 = atoll(ln.c_str() + 22);
        else if (ln.startsWith("#EXT-X-TARGETDURATION:")) target = max(1, atoi(ln.c_str() + 22));
        else if (ln.startsWith("#EXT-X-STREAM-INF")) {             // master playlist: note this variant's bitrate
          int k = ln.indexOf("BANDWIDTH="); bw = k >= 0 ? atol(ln.c_str() + k + 10) : 0;
        }
        else if (ln[0] != '#') {
          if (bw >= 0) { if (bw <= 256000 && bw > bestBw) { bestBw = bw; variant = ln; } bw = -1; }   // best up to 256 kbps
          else if (cnt < 24) uris[cnt++] = ln;
        }
      }
      if (variant.length()) { pl = resolveUrl(real, variant); continue; }   // master: follow a variant
      if (!cnt) { f->nap(target * 500); continue; }
      if (lastSeq < 0) lastSeq = seq0 + cnt - 1 - 3;              // start 3 segments behind the live edge
      if (lastSeq < seq0 - 1) lastSeq = seq0 - 1;                  // fell behind the window: jump
      bool any = false;
      for (int i = 0; i < cnt && !f->quit; i++) {
        long long sq = seq0 + i; if (sq <= lastSeq) continue;
        f->ts.have = 0;                                             // segments start on a packet boundary
        httpsGet(resolveUrl(real, uris[i]), [&](const uint8_t *b, int n) {
          f->ts.push(b, n, [&](const uint8_t *p, int k) { f->sendAll(p, k); });
        }, f->quit);
        lastSeq = sq; any = true; f->hlsSegs++;
      }
      f->nap(any ? target * 500 : target * 300);
    }
    f->task = NULL; vTaskDelete(NULL);
  }
  bool openHls(const char *url) {            // true once audio is flowing
    aac = true; hlsUrl = url;
    adec = AACInitDecoder(); if (!adec) return false;
    if (!startOut(160 * 1024)) return false;
    xTaskCreatePinnedToCore(hlsTask, "fipHls", 12288, this, 6, &task, 0);
    uint32_t t0 = millis();
    while (buffered() < 8 * 1024 && !lost && millis() - t0 < 12000) delay(50);
    return buffered() >= 8 * 1024 && !lost;
  }
  bool open(const char *url, int hops = 0) {
    String u = url; if (!u.startsWith("https://")) return false;
    int sl = u.indexOf('/', 8); String host = u.substring(8, sl < 0 ? u.length() : sl), path = sl < 0 ? "/" : u.substring(sl);
    cli.setInsecure(); cli.setTimeout(5000);
    if (!cli.connect(host.c_str(), 443)) return false;
    // HTTP/1.0: a server may not answer with chunked transfer; and no ICY metadata, please
    cli.print("GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: Einkster\r\nIcy-MetaData: 0\r\nConnection: close\r\n\r\n");
    String status = cli.readStringUntil('\n'); String loc; bool chunked = false; int metaInt = 0;
    while (cli.connected()) {
      String h = cli.readStringUntil('\n'); h.trim(); if (!h.length()) break;
      String l = h; l.toLowerCase();
      if (l.startsWith("location:")) { loc = h.substring(9); loc.trim(); }
      if (l.startsWith("transfer-encoding:") && l.indexOf("chunked") > 0) chunked = true;
      if (l.startsWith("icy-metaint:")) metaInt = l.substring(12).toInt();
    }
    clean.begin(chunked, metaInt);
    if ((status.indexOf(" 301") > 0 || status.indexOf(" 302") > 0) && loc.length() && hops < 2) { cli.stop(); return open(loc.c_str(), hops + 1); }
    if (status.indexOf(" 200") < 0) { cli.stop(); return false; }
    dec = MP3InitDecoder(); if (!dec) return false;
    if (!startOut(48 * 1024)) return false;
    xTaskCreatePinnedToCore(netTask, "fipNet", 6144, this, 6, &task, 0);
    return true;
  }
  bool startOut(size_t sbSize) {             // the network buffer, the speaker cushion and its task
    sbMem = (uint8_t *)heap_caps_malloc(sbSize + 1, MALLOC_CAP_SPIRAM);
    pcmMem = (uint8_t *)heap_caps_malloc(64 * 1024 + 1, MALLOC_CAP_SPIRAM);
    if (!sbMem || !pcmMem) return false;
    sb = xStreamBufferCreateStatic(sbSize, 1, sbMem, &sbCtl);
    pcmQ = xStreamBufferCreateStatic(64 * 1024, 4, pcmMem, &pcmCtl);
    xTaskCreatePinnedToCore(outTask, "fipOut", 4096, this, 10, &outT, 1);
    memset(hist, 0, sizeof hist); hpInit();
    return true;
  }
  size_t buffered() { return sb ? xStreamBufferBytesAvailable(sb) : 0; }
  void design(int rate) {                    // low-pass for 16 kHz output, Blackman window
    if (!coef) coef = (float (*)[RS_TAPS])heap_caps_malloc(sizeof(float) * RS_PHASES * RS_TAPS, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!coef) coef = (float (*)[RS_TAPS])heap_caps_malloc(sizeof(float) * RS_PHASES * RS_TAPS, MALLOC_CAP_SPIRAM);
    rsRate = rate; float fc = 6500.0f / rate;   // flat to ~6 kHz, gone before the 8 kHz fold
    for (int ph = 0; ph < RS_PHASES; ph++) {
      float frac = ph / (float)RS_PHASES, sum = 0;
      for (int k = 0; k < RS_TAPS; k++) {
        float x = k - (RS_TAPS / 2 - 1) - frac;                  // distance from the output instant
        float sinc = fabsf(x) < 1e-6f ? 2 * fc : sinf(2 * PI * fc * x) / (PI * x);
        float w = 0.42f - 0.5f * cosf(2 * PI * (k + 1 - frac) / (RS_TAPS + 1)) + 0.08f * cosf(4 * PI * (k + 1 - frac) / (RS_TAPS + 1));
        coef[ph][k] = sinc * w; sum += coef[ph][k];
      }
      for (int k = 0; k < RS_TAPS; k++) coef[ph][k] /= sum;      // unity gain
    }
  }
  // decode one frame; returns frames decoded (0 = waiting for data, -1 = stream lost)
  int step(float gain, bool audible) {
    if (left < 2048 || needMore) {           // top up from the buffer
      needMore = false;
      if (left && ptr != in) memmove(in, ptr, left);
      ptr = in;
      left += xStreamBufferReceive(sb, in + left, sizeof in - left, pdMS_TO_TICKS(20));
      if (left < 1024) return lost ? -1 : 0;
    }
    int frames, nch, rate;
    if (aac) {                                 // HLS: ADTS AAC frames
      int off = AACFindSyncWord(ptr, left);
      if (off < 0) { nJunk += left; left = 0; return 0; }
      nJunk += off; ptr += off; left -= off;
      uint8_t *before = ptr; int beforeLeft = left;
      int err = AACDecode(adec, &ptr, &left, pcm);
      if (err == ERR_AAC_INDATA_UNDERFLOW) { ptr = before; left = beforeLeft; if (left < (int)sizeof in) needMore = true; return 0; }
      if (err != ERR_AAC_NONE) { nBad++; if (ptr == before && left > 0) { ptr++; left--; } return 0; }
      AACFrameInfo fi; AACGetLastFrameInfo(adec, &fi);
      nOk++; srate = fi.sampRateOut; nch = fi.nChans; rate = fi.sampRateOut;
      frames = fi.outputSamps / max(1, fi.nChans);
    } else {                                   // Icecast: MP3 frames
      int off = MP3FindSyncWord(ptr, left);
      if (off < 0) { nJunk += left; left = 0; return 0; }
      nJunk += off;                            // bytes between frames: should stay at zero
      ptr += off; left -= off;
      uint8_t *before = ptr; int beforeLeft = left;
      int err = MP3Decode(dec, &ptr, &left, pcm, 0);
      if (err == ERR_MP3_INDATA_UNDERFLOW) { ptr = before; left = beforeLeft; if (left < (int)sizeof in) { needMore = true; } return 0; }
      if (err == ERR_MP3_MAINDATA_UNDERFLOW) return 0;            // bit reservoir filling after a resync: normal
      if (err != ERR_MP3_NONE) { nBad++; if (ptr == before && left > 0) { ptr++; left--; } return 0; }  // resync on the next frame
      MP3FrameInfo fi; MP3GetLastFrameInfo(dec, &fi);
      nOk++; kbps = fi.bitrate / 1000; srate = fi.samprate; nch = fi.nChans; rate = fi.samprate;
      frames = fi.outputSamps / max(1, fi.nChans);
    }
    if (!audible || frames <= 0) return frames;          // before the alarm time: decode and drop
    if (frames > 2048) frames = 2048;
    float *x = hist + RS_TAPS;               // history (last RS_TAPS input samples) + this frame, mono
    if (radioTest == 2) testTone(x, frames, rate);                   // test B: same load, a clean tone
    else for (int i = 0; i < frames; i++) x[i] = nch == 2 ? (pcm[2 * i] + pcm[2 * i + 1]) * 0.5f : pcm[i];
    emit(frames, rate, gain);
    return frames;
  }
  // resample hist[RS_TAPS .. RS_TAPS+frames) from `rate` to 16 kHz into the cushion
  void emit(int frames, int rate, float gain) {
    if (rate != rsRate) design(rate);
    float *x = hist + RS_TAPS;
    static int16_t out[1152 * 2]; int n = 0;
    float stepIn = rate / (float)EK_SAMPLE_RATE;
    while (n < 1152) {
      int i0 = (int)floorf(rsPos); float fr = rsPos - i0;
      if (i0 + RS_TAPS / 2 > frames - 1) break;                  // needs samples from the next frame
      const float *c = coef[(int)(fr * RS_PHASES) % RS_PHASES];
      const float *src = x + i0 - (RS_TAPS / 2 - 1);
      float acc = 0; for (int k = 0; k < RS_TAPS; k++) acc += src[k] * c[k];
      int16_t v = (int16_t)constrain(speaker(acc, gain), -32000.0f, 32000.0f);
      out[2 * n] = out[2 * n + 1] = v; n++;
      rsPos += stepIn;
    }
    rsPos -= frames;
    memmove(hist, hist + frames, RS_TAPS * sizeof(float));      // keep the tail for the next frame
    if (n) xStreamBufferSend(pcmQ, out, n * 4, portMAX_DELAY);   // whole stereo frames only: never split
  }
  String stats() {                           // "44.1k 128k err 0.0%" (+ what was stripped)
    char b[48]; float e = (nOk + nBad) ? 100.0f * nBad / (nOk + nBad) : 0;
    if (aac) snprintf(b, sizeof b, "HLS %.1fk err %.1f%%", srate / 1000.0f, e);
    else snprintf(b, sizeof b, "%.1fk %dk err %.1f%%%s%s", srate / 1000.0f, kbps, e, clean.chunked ? " chk" : "", clean.metaInt ? " icy" : "");
    return String(b);
  }
  size_t queued() { return pcmQ ? xStreamBufferBytesAvailable(pcmQ) : 0; }
  void close() {
    quit = true; uint32_t t0 = millis(); while ((task || outT) && millis() - t0 < 20000) delay(10);   // never free under a live task
    cli.stop(); if (dec) MP3FreeDecoder(dec); dec = NULL;
    if (adec) AACFreeDecoder(adec); adec = NULL;
    if (coef) free(coef); coef = NULL;
    if (sb) vStreamBufferDelete(sb); sb = NULL;
    if (pcmQ) vStreamBufferDelete(pcmQ); pcmQ = NULL;
    free(sbMem); free(pcmMem); sbMem = pcmMem = NULL;
  }
};

// FIP: HLS first (no pre-roll ad), Icecast MP3 if HLS cannot start. f must be freshly constructed.
static bool fipOpen(FipStream *f) {
  if (f->openHls(FIP_HLS)) return true;
  f->close(); f->~FipStream(); new (f) FipStream();
  return f->open(FIP_URL);
}

// ---- the ring
static void ringScreen(int kind, uint32_t ev) {
  epd->EPD_Clear();
  time_t e = ev; struct tm t; localtime_r(&e, &t);
  textAt(EK_BIG, 100, 30, kind == EV_TIMER ? L("TIMER", "MINUTEUR") : L("ALARM", "R\xC3\x89" "VEIL"));
  const int W = 38, H = 74, K = 9, Y = 72;
  if (kind == EV_TIMER) { textAt(EK_MID, 100, 100, L("time's up", "c\xE2\x80\x99" "est l\xE2\x80\x99" "heure")); }
  else {
    digit(8, Y, t.tm_hour / 10, W, H, K); digit(52, Y, t.tm_hour % 10, W, H, K);
    rect(96, Y + 20, 8, 8, true); rect(96, Y + H - 28, 8, 8, true);
    digit(110, Y, t.tm_min / 10, W, H, K); digit(154, Y, t.tm_min % 10, W, H, K);
  }
  textAt(EK_SMALL, 100, 164, kind == EV_TIMER ? L("tap: stop", "touchez\xC2\xA0" ": stop") : L("tap: snooze 9 min", "touchez\xC2\xA0" ": +9\xC2\xA0" "min"));
  textAt(EK_SMALL, 100, 182, L("hold: stop", "maintenir\xC2\xA0" ": stop"));
  show();
}
static void audioEnsure();
static void ringRun(int kind, uint32_t ev) {
  audioEnsure();
  ringScreen(kind, ev);
  int tone = kind == EV_TIMER ? tmTone : alTone;
  FipStream *fip = NULL;
  if (tone == TONE_FIP) {                    // connect during the lead time; any failure: chime instead
    fip = (FipStream *)heap_caps_calloc(1, sizeof(FipStream), MALLOC_CAP_SPIRAM);
    if (fip) new (fip) FipStream();
    if (!fip || !wifiUp()) { if (fip) { fip->~FipStream(); free(fip); } fip = NULL; tone = TONE_CHIME; }
    else {
      WiFi.setSleep(false);                  // no radio naps while streaming: they cause the gaps
      if (!fipOpen(fip)) { fip->close(); fip->~FipStream(); free(fip); fip = NULL; tone = TONE_CHIME; }
    }
  }
  bool primed = false;                       // FIP: let the buffer fill before the first note
  esp_codec_dev_set_out_vol(pbDev(), 100.0);
  Synth syn; bool stopped = false, snoozed = false;
  while (!stopped && !snoozed) {
    int32_t since = (int32_t)time(NULL) - (int32_t)ev;       // < 0 while waiting for the minute
    if (since > RING_MAX_S) break;
    float gain = since < 0 ? 0 : min(1.0f, 0.15f + since / 30.0f * 0.85f);
    int t2 = (fip == NULL && since > ESCALATE_S) ? TONE_BEACON : tone;   // chime turns into beacon
    if (fip) {
      if (since >= 0 && !primed) {           // up to 3 s to reach ~1.5 s of audio in hand
        uint32_t w0 = millis(); while (fip->buffered() < 24 * 1024 && !fip->lost && millis() - w0 < 3000) delay(20);
        primed = true;
      }
      if (fip->step(gain, since >= 0) < 0) { fip->close(); fip->~FipStream(); free(fip); fip = NULL; tone = TONE_CHIME; }
    } else if (since >= 0) playChunk(t2, syn, t2 == TONE_BEACON ? 1.0f : gain, 256);
    else delay(50);
    if (since >= 0 && touched()) {           // tap: snooze (timer: stop), hold: stop
      uint32_t t0 = millis(); while (touched() && millis() - t0 < 600) delay(15);   // bounded
      if (millis() - t0 >= 600 || kind == EV_TIMER) stopped = true; else snoozed = true;
      waitRelease();
    }
    if (digitalRead(PWR_BTN) == LOW) stopped = true;
  }
  if (fip) { fip->close(); fip->~FipStream(); free(fip); }
  WiFi.setSleep(true);
  pbRestore();
  if (kind == EV_TIMER) { rtcTimerEnd = 0; prefs.putUInt("tmEnd", 0); }
  else {
    if (kind == EV_ALARM) rtcLastAlarmAt = ev;
    rtcSnoozeAt = snoozed ? (uint32_t)time(NULL) + SNOOZE_S : 0;
    if (!snoozed && kind == EV_ALARM && alRep == 0) { alOn = false; prefs.putBool("alOn", false); }
  }
  tile = T_CLOCK; prefs.putInt("tile", tile);
  screenIdle();
  lastTouch = millis();
}
static bool ringIfDue() {                    // call from the loop and at every wake
  int k; uint32_t ev = nextEvent(k);
  if (!ev || (int64_t)time(NULL) + leadFor(k) < (int64_t)ev) return false;
  ringRun(k, ev);
  return true;
}

// ---- the radio tile: FIP, one station, a 70s pocket receiver
static int radVol = 6;                       // 0..10, remembered
static String radStats;                      // on-air readout from the stream
static void radioDraw(int st) {              // 0 off, 1 tuning, 2 on air, 3 no signal, 4 Wi-Fi not set up
  if (st == 0 && !wifiConfigured()) st = 4;
  epd->EPD_Clear(); tileDots();
  rect(8, 22, 184, 2, true); rect(8, 170, 184, 2, true); rect(8, 22, 2, 150, true); rect(190, 22, 2, 150, true);   // the case
  for (int y = 58; y <= 142; y += 7) for (int x = 12; x <= 96; x += 7) {                                          // round speaker grille
    int dx = x - 54, dy = y - 100; if (dx * dx + dy * dy <= 40 * 40) disc(x, y, 2);
  }
  rect(98, 34, 84, 40, true); rect(100, 36, 80, 36, false);                                                      // tuning window
  for (int f = 88; f <= 108; f += 2) { int x = 102 + (f - 88) * 76 / 20; rect(x, 36, 1, f % 10 == 0 ? 10 : 5, true); }
  textAt(EK_SMALL, 110, 50, "90"); textAt(EK_SMALL, 148, 50, "100");
  int nx = 102 + (int)((105.1f - 88) * 76 / 20); rect(nx, 36, 2, 36, true);                                   // the needle on 105.1
  textAt(EK_BIG, 140, 80, "FIP");
  if (st == 2) {                             // on air: a lit label and the volume
    rect(104, 122, 72, 20, true); textAt(EK_SMALL, 140, 123, L("ON AIR", "DIRECT"));   // drawn white on black below
    for (int i = 0; i < 10; i++) {           // step 10, the loud step: a solid tall bar
      int h = i == 9 ? 20 : 4 + i, w = i == 9 ? 7 : 5;
      if (i < radVol) rect(106 + i * 7, 164 - h, w, h, true); else rect(106 + i * 7, 163, 5, 1, true);
    }
  } else if (st == 4) { textAt(EK_SMALL, 140, 122, L("Wi-Fi not", "Wi-Fi non")); textAt(EK_SMALL, 140, 139, L("set up", "configur\xC3\xA9")); }
  else textAt(EK_SMALL, 140, 128, st == 1 ? L("tuning...", "r\xC3\xA9" "glage...") : st == 3 ? L("no signal", "sans signal") : "105.1");
  if (st == 4) textAt(EK_SMALL, 100, 178, L("hold: set it up", "maintenir\xC2\xA0" ": configurer"));
  else if (st != 1) textAt(EK_SMALL, 100, 178, st == 2 ? L("tap: stop \xC2\xB7 \xE2\x80\xB9 \xE2\x80\xBA volume", "centre\xC2\xA0" ": stop \xC2\xB7" " \xE2\x80\xB9" " \xE2\x80\xBA" " vol.") : st == 3 ? L("tap to retry", "touchez\xC2\xA0" ": r\xC3\xA9" "essayer") : L("tap to play", "touchez pour \xC3\xA9" "couter"));
  if (st == 2) { static uint16_t t[16]; int n = decodeUtf8(L("ON AIR", "DIRECT"), t, 16), w = 0;          // the label in paper-white
    for (int i = 0; i < n; i++) w += glyphW(EK_SMALL, t[i]);
    int x = 140 - w / 2; for (int i = 0; i < n; i++) { glyphInk(EK_SMALL, x, 123, t[i], false); x += glyphW(EK_SMALL, t[i]); } }
  partialOnly = (st == 1 || st == 2);        // while sound plays: quick partial updates only (a full one stalls the audio)
  show(); partialOnly = false; onIdle = true;
}
static float radioBoost() { return radVol >= 10 ? 2.24f : 1.0f; }   // step 10: +7 dB above the old maximum
static void radioVolume() { esp_codec_dev_set_out_vol(pbDev(), radVol >= 9 ? 100.0 : 40.0 + radVol * 6.0); }
static void testScreen(const char *l1, const char *l2) {
  epd->EPD_Clear(); textAt(EK_MID, 100, 40, l1); textBlock(l2, 8, 80, 184, 70, false);
  textAt(EK_SMALL, 100, 178, "tap: stop"); show();
}
static bool stopTouch() {                    // any tap or PWR ends a test
  if (touched()) { waitRelease(); return true; }
  if (digitalRead(PWR_BTN) == LOW) { while (digitalRead(PWR_BTN) == LOW) delay(10); return true; }
  return false;
}
static void radioTestA() {                   // tone -> resampler -> cushion -> speaker, radio off
  audioEnsure(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  testScreen("test A", "440 Hz tone, same audio chain, Wi-Fi off");
  FipStream *f = (FipStream *)heap_caps_calloc(1, sizeof(FipStream), MALLOC_CAP_SPIRAM);
  if (f) {
    new (f) FipStream(); radioTest = 1; radioVolume();
    if (f->startOut(48 * 1024)) { uint32_t t0 = millis(); while (millis() - t0 < 60000) {
      testTone(f->hist + RS_TAPS, 1152, 44100); f->emit(1152, 44100, 1.0f);
      if (stopTouch()) break; } }
    f->close(); f->~FipStream(); free(f);
  }
  radioTest = 0; pbRestore(); radioDraw(0); lastTouch = millis();
}
static void radioRun() {
  audioEnsure();
  if (radioTest == 2) testScreen("test B", "440 Hz tone, while FIP streams and decodes (Wi-Fi busy)");
  else radioDraw(1);
  FipStream *f = (FipStream *)heap_caps_calloc(1, sizeof(FipStream), MALLOC_CAP_SPIRAM);
  if (f) new (f) FipStream();
  uint32_t flT = millis(); bool flAir = false;
  bool ok = f && wifiUp();
  if (ok) { WiFi.setSleep(false); ok = fipOpen(f); }
  FL("radio", ok ? String("on air over ") + (f->aac ? "HLS" : "Icecast (the fallback, may carry an ad)") + " after " + flSecs(millis() - flT)
                 : String("no signal after ") + flSecs(millis() - flT));
  if (ok) {                                  // fill the cushions before the first note
    flAir = true; flT = millis();
    uint32_t t0 = millis(); while (f->buffered() < 16 * 1024 && !f->lost && millis() - t0 < 5000) delay(20);
    radioVolume(); f->boost = radioBoost(); if (radioTest != 2) radioDraw(2);
    bool tDown = false; uint32_t tStart = 0; int sx = 100;
    radStats = "";
    for (;;) {
      if (f->step(1.0f, true) < 0) { ok = false; break; }
      flPoll(true);                          // BOOT = a mark, without a note over the music

      int tx, ty; bool down = touchXY(tx, ty);
      if (down && !tDown) { tDown = true; tStart = millis(); sx = TOUCH_FLIP_X ? 199 - tx : tx; }
      else if (!down && tDown) {
        tDown = false;
        if (millis() - tStart >= 600 || (sx >= 67 && sx <= 133)) break;           // hold or middle: stop
        radVol = constrain(radVol + (sx < 67 ? -1 : 1), 0, 10);                  // sides: volume
        prefs.putInt("radVol", radVol); radioVolume(); f->boost = radioBoost(); if (radioTest != 2) radioDraw(2);
      }
      if (digitalRead(PWR_BTN) == LOW) { while (digitalRead(PWR_BTN) == LOW) delay(10); break; }
    }
  }
  if (flAir) FL("radio", String(ok ? "stopped by hand" : "STREAM LOST") + " after " + flSecs(millis() - flT) + " on air");
  if (f) { f->close(); f->~FipStream(); free(f); }
  WiFi.setSleep(true); pbRestore(); radioTest = 0; radStats = "";
  radioDraw(ok ? 0 : 3);
  lastTouch = millis();
}



// ================================================================ CLIM@TE: one weather engine, both editions
// Open-Meteo directly: forecast, marine (where the sea exists) and air quality. No key, no account.
// Everything the faces need is parsed once into wx (kept through deep sleep); the raw JSON is dropped.
static volatile bool wxQuit = false;
RTC_DATA_ATTR static uint8_t rtcWxMiss;        // the last forecast a person asked for did not come: the face says so
static int wxArr(const String &b, const char *sect, const char *key, float *out, int max, int from = 0) {
  int s0 = b.indexOf(String("\"") + sect + "\":{", from); if (s0 < 0) return 0;
  int k = b.indexOf(String("\"") + key + "\":[", s0); if (k < 0) return 0;
  const char *c = b.c_str() + b.indexOf('[', k) + 1; int n = 0;
  while (*c && *c != ']' && n < max) {
    if (!strncmp(c, "null", 4)) { out[n++] = NAN; c += 4; }
    else if (*c == '"') { out[n++] = NAN; c = strchr(c + 1, '"'); if (!c) break; c++; }   // a string: skip
    else out[n++] = strtof(c, (char **)&c);
    while (*c == ',' || *c == ' ') c++;
  }
  return n;
}
static int wxStrMin(const String &b, const char *sect, const char *key, int idx) {   // "…T07:18" -> 438 (minutes)
  int s0 = b.indexOf(String("\"") + sect + "\":{"); if (s0 < 0) return -1;
  int k = b.indexOf(String("\"") + key + "\":[", s0); if (k < 0) return -1;
  int q = b.indexOf('[', k) + 1;
  for (int i = 0; i < idx; i++) { q = b.indexOf(',', q); if (q < 0) return -1; q++; }
  int t = b.indexOf('T', q); if (t < 0 || t - q > 20) return -1;
  return b.substring(t + 1, t + 3).toInt() * 60 + b.substring(t + 4, t + 6).toInt();
}
static bool wxGet(const String &url, String &out) {
  out = ""; wxQuit = false; uint32_t flT = millis();
  bool ok = FipStream::httpsGet(url, [&](const uint8_t *d, int n) { if (out.length() < 40000) out.concat((const char *)d, n); }, wxQuit);
  FL("fetch", flHost(url) + (ok ? " ok, " : " FAILED, ") + String(out.length()) + " bytes in " + flSecs(millis() - flT));
  return ok;
}
static bool wxFetchAll() {                   // true when a forecast came in; marine and air ride along
  if (!wifiConfigured() || offlineNow()) return false;
  if (!wifiUp()) { rtcNetDownUntil = nowS() + NET_BACKOFF_S; return false; }
  String b;
  if (!wxGet("https://api.open-meteo.com/v1/forecast?latitude=" EK_LAT "&longitude=" EK_LON
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day,wind_speed_10m,wind_direction_10m,wind_gusts_10m,pressure_msl,uv_index"
             "&minutely_15=precipitation&forecast_minutely_15=8"
             "&hourly=temperature_2m,precipitation_probability,weather_code,wind_speed_10m,pressure_msl&past_hours=72&forecast_hours=36"
             "&daily=temperature_2m_max,temperature_2m_min,weather_code,sunrise,sunset,precipitation_sum,uv_index_max&forecast_days=5"
             "&timezone=auto", b)) return false;
  int cur = b.indexOf("\"current\":{"); if (cur < 0) return false;
  WxData &w = wx;
  w.t = jnum(b, "temperature_2m", cur); w.rh = jnum(b, "relative_humidity_2m", cur); w.feels = jnum(b, "apparent_temperature", cur);
  float code = jnum(b, "weather_code", cur), day = jnum(b, "is_day", cur);
  w.code = isnan(code) ? 0 : (int)code; w.day = isnan(day) ? 1 : (int)day;
  w.wind = jnum(b, "wind_speed_10m", cur); w.gust = jnum(b, "wind_gusts_10m", cur);
  float wd = jnum(b, "wind_direction_10m", cur); w.wdir = isnan(wd) ? -1 : (int)wd;
  w.pmsl = jnum(b, "pressure_msl", cur); w.uv = jnum(b, "uv_index", cur);
  float r[8]; int nr = wxArr(b, "minutely_15", "precipitation", r, 8);
  for (int i = 0; i < 8; i++) w.rain15[i] = i < nr && !isnan(r[i]) ? r[i] : 0;
  static float ht[108], hp[108], hc[108], hw[108], hpr[108];
  int nh = wxArr(b, "hourly", "temperature_2m", ht, 108);
  wxArr(b, "hourly", "precipitation_probability", hp, 108); wxArr(b, "hourly", "weather_code", hc, 108);
  wxArr(b, "hourly", "wind_speed_10m", hw, 108); int np = wxArr(b, "hourly", "pressure_msl", hpr, 108);
  time_t e = time(NULL); struct tm tm; localtime_r(&e, &tm);
  int now = 72;                                                      // past_hours=72: index 72 is this hour
  for (int i = 0; i < 12; i++) {
    int k = now + 1 + i; if (k >= nh) k = nh - 1;
    w.hh[i] = (tm.tm_hour + 1 + i) % 24; w.ht[i] = ht[k]; w.hp[i] = isnan(hp[k]) ? 0 : (uint8_t)hp[k];
    w.hcode[i] = isnan(hc[k]) ? 0 : (uint8_t)hc[k]; w.hwind[i] = isnan(hw[k]) ? 0 : (uint8_t)hw[k];
  }
  for (int i = 0; i < 96; i++) { int k = i + 12; w.pres[i] = (k < np && !isnan(hpr[k])) ? (int16_t)lroundf(hpr[k] * 10) : 0; }   // 60 h back .. 36 h ahead
  float dmx[5], dmn[5], dc[5], dp[5], du[5];
  int nd = wxArr(b, "daily", "temperature_2m_max", dmx, 5);
  wxArr(b, "daily", "temperature_2m_min", dmn, 5); wxArr(b, "daily", "weather_code", dc, 5);
  wxArr(b, "daily", "precipitation_sum", dp, 5); wxArr(b, "daily", "uv_index_max", du, 5);
  for (int i = 0; i < 5; i++) {
    w.dmax[i] = i < nd ? dmx[i] : NAN; w.dmin[i] = i < nd ? dmn[i] : NAN; w.dcode[i] = i < nd && !isnan(dc[i]) ? (uint8_t)dc[i] : 0;
    w.dprec[i] = i < nd && !isnan(dp[i]) ? dp[i] : 0; w.duv[i] = i < nd && !isnan(du[i]) ? du[i] : 0;
    w.sunrise[i] = wxStrMin(b, "daily", "sunrise", i); w.sunset[i] = wxStrMin(b, "daily", "sunset", i);
  }
  w.at = nowS(); w.valid = true; rtcWxMiss = 0;
  // ---- marine: only where the model has a sea. Once it answers with nothing, we stop asking.
  if (!w.noMarine) {
    String m;
    if (wxGet("https://marine-api.open-meteo.com/v1/marine?latitude=" EK_LAT "&longitude=" EK_LON
              "&current=wave_height,wave_period,wave_direction,sea_surface_temperature"
              "&minutely_15=sea_level_height_msl&forecast_minutely_15=192&timezone=auto", m)) {
      int mc = m.indexOf("\"current\":{");
      float wh = mc < 0 ? NAN : jnum(m, "wave_height", mc);
      if (isnan(wh)) { if (m.indexOf("\"error\"") >= 0 || m.indexOf("null") >= 0) w.noMarine = true; w.marine = false; }
      else {
        w.marine = true; w.wave = wh; w.wperiod = jnum(m, "wave_period", mc);
        float d = jnum(m, "wave_direction", mc); w.wavedir = isnan(d) ? -1 : (int)d; w.sst = jnum(m, "sea_surface_temperature", mc);
        static float sea[192]; int ns = wxArr(m, "minutely_15", "sea_level_height_msl", sea, 192);
        for (int i = 0; i < 192; i++) w.sea[i] = i < ns && !isnan(sea[i]) ? (int16_t)lroundf(sea[i] * 100) : INT16_MIN;
        w.seaT0 = (uint32_t)e - (uint32_t)(tm.tm_min % 15) * 60 - tm.tm_sec;   // this quarter hour
      }
    }
  }
  // ---- air quality (Europe: pollen too)
  String a;
  w.air = false;
  if (wxGet("https://air-quality-api.open-meteo.com/v1/air-quality?latitude=" EK_LAT "&longitude=" EK_LON
            "&current=european_aqi,pm2_5,pm10,ozone,nitrogen_dioxide,alder_pollen,birch_pollen,grass_pollen,mugwort_pollen,olive_pollen,ragweed_pollen&timezone=auto", a)) {
    int ac = a.indexOf("\"current\":{");
    if (ac >= 0) {
      float aqi = jnum(a, "european_aqi", ac);
      if (!isnan(aqi)) {
        w.air = true; w.aqi = (int)aqi; w.pm25 = jnum(a, "pm2_5", ac); w.pm10 = jnum(a, "pm10", ac);
        w.o3 = jnum(a, "ozone", ac); w.no2 = jnum(a, "nitrogen_dioxide", ac);
        const char *pk[6] = { "alder_pollen", "birch_pollen", "grass_pollen", "mugwort_pollen", "olive_pollen", "ragweed_pollen" };
        for (int i = 0; i < 6; i++) { float g = jnum(a, pk[i], ac); w.pollen[i] = isnan(g) ? 0 : g < 1 ? 0 : g < 20 ? 1 : g < 80 ? 2 : 3; }   // grains/m3 -> 0..3
      }
    }
  }
  return true;
}
static bool wxFresh() { return wx.valid && nowS() - wx.at < WX_STALE_S; }
static String hhmm(int m) { if (m < 0) return "--:--"; char b[8]; snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60); return b; }
static String rainLine() {                     // the one line the clock shows
  if (!wx.valid) return L("no forecast yet", "pas de pr\xC3\xA9" "visions");
  bool snow = wx.code >= 71 && wx.code <= 77;
  int first = -1; for (int i = 0; i < 8; i++) if (wx.rain15[i] >= 0.05f) { first = i; break; }
  if (first < 0) return L("dry for 2h+", "sec pour 2\xC2\xA0" "h+");
  if (first == 0) return snow ? L("snowing now", "il neige") : L("raining now", "il pleut");
  int m = first * 15;
  if (uiFr) return String(snow ? "neige" : "pluie") + "\xC2\xA0" ": " + (m >= 60 ? String(m / 60) + "\xC2\xA0" "h\xC2\xA0" + (m % 60 < 10 ? "0" : "") + String(m % 60) : String(m) + " min");
  return String(snow ? "snow" : "rain") + " in ~" + String(m) + " min";
}
static const char *compass(int d) { static const char *N[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" }; return d < 0 ? "--" : N[((d + 22) / 45) % 8]; }
// ---- the barometer: the local sensor's pressure when fitted, else the model's; 3-hour trend; Zambretti's verdict
static float presNow() { return pLocalFresh() ? wx.pLocal : wx.pmsl; }
// v0.25.3: the 3-hour change, from one source only. The sensor's own reading 3 hours ago when it has one, else the model's
// own change over the same 3 hours. v0.25.2 set the sensor now against the model 3 hours ago, and showed a fall that was
// only the gap between the two.
static float presTrend() {
  if (pLocalFresh()) { float a = phAgo(time(NULL), 3 * 3600); if (!isnan(a)) return wx.pLocal - a; }
  return (wx.pres[60] && wx.pres[57]) ? (wx.pres[60] - wx.pres[57]) / 10.0f : NAN;
}
static int zambretti(float p, float trend, int &dir) {   // 1..26, dir -1 falling, 0 steady, 1 rising
  if (isnan(p)) return 0;
  dir = trend <= -1.6f ? -1 : trend >= 1.6f ? 1 : 0;
  float z = dir < 0 ? 127 - 0.12f * p : dir > 0 ? 185 - 0.16f * p : 144 - 0.13f * p;
  return constrain((int)lroundf(z), 1, 26);
}
static const char *ZAMB_EN[] = { "", "settled fine", "fine weather", "becoming fine", "fine, becoming less settled", "fine, showers possible",
  "fairly fine, improving", "fairly fine, showers early", "fairly fine, showers later", "showery early, improving", "changeable, improving",
  "fairly fine, showers likely", "rather unsettled, clearing", "unsettled, probably improving", "showery, bright intervals", "showery, becoming unsettled",
  "changeable, some rain", "unsettled, short fine intervals", "unsettled, rain later", "unsettled, rain at times", "very unsettled, rain",
  "rain at times, worse later", "rain at times, becoming unsettled", "rain at frequent intervals", "very unsettled, rain", "stormy, may improve", "stormy, much rain" };
static const char *ZAMB_FR[] = { "", "beau temps \xC3\xA9" "tabli", "beau temps", "s\xE2\x80\x99" "am\xC3\xA9" "liore", "beau, moins stable", "beau, averses possibles",
  "assez beau, am\xC3\xA9" "lioration", "assez beau, averses t\xC3\xB4" "t", "assez beau, averses plus tard", "averses t\xC3\xB4" "t, am\xC3\xA9" "lioration", "variable, am\xC3\xA9" "lioration",
  "assez beau, averses probables", "instable, \xC3\xA9" "claircies", "instable, am\xC3\xA9" "lioration probable", "averses, \xC3\xA9" "claircies", "averses, se d\xC3\xA9" "grade",
  "variable, un peu de pluie", "instable, br\xC3\xA8" "ves \xC3\xA9" "claircies", "instable, pluie plus tard", "instable, pluie par moments", "tr\xC3\xA8" "s instable, pluie",
  "pluie par moments, pire ensuite", "pluie par moments, se d\xC3\xA9" "grade", "pluie fr\xC3\xA9" "quente", "tr\xC3\xA8" "s instable, pluie", "temp\xC3\xAA" "te, peut s\xE2\x80\x99" "am\xC3\xA9" "liorer", "temp\xC3\xAA" "te, beaucoup de pluie" };
// ---- tides: next high and low from the 15-minute sea level series, with the local chart datum
static bool tideNext(bool high, int &atMin, float &height) {   // atMin: minutes from now; height above chart datum
  if (!wx.marine) return false;
  uint32_t now = nowS(); int i0 = (int)((now - wx.seaT0) / 900); if (i0 < 1) i0 = 1;
  for (int i = i0; i < 191; i++) {
    int16_t a = wx.sea[i - 1], b = wx.sea[i], c = wx.sea[i + 1];
    if (a == INT16_MIN || b == INT16_MIN || c == INT16_MIN) continue;
    if (high ? (b >= a && b > c) : (b <= a && b < c)) {
      float den = (float)(a - 2 * b + c), off = den ? 0.5f * (a - c) / den : 0;     // parabola through the three points
      atMin = (int)((wx.seaT0 + (uint32_t)(i * 900) - now) / 60 + off * 15);
      height = b / 100.0f + EK_TIDE_DATUM; return true;
    }
  }
  return false;
}
// ---------------------------------------------------------------- CLIM@TE faces and pages
static void textRight(const EkFont &f, int xr, int y, const String &s) { textLeft(f, xr - textWidth(f, s), y, s); }
static void bell(int bx, int by) { rect(bx + 3, by, 4, 2, true); rect(bx + 1, by + 2, 8, 6, true); rect(bx, by + 8, 10, 2, true); rect(bx + 4, by + 10, 2, 2, true); }
static void drawClock() {
  if (!timeValid()) {
    textBlock(L("clock\nwaiting for\ninternet time", "horloge\nen attente de\nl\xE2\x80\x99" "heure internet"), 8, 30, 184, 130, false);
    return;
  }
  time_t e = time(NULL); struct tm t; localtime_r(&e, &t);
  const int W = 26, H = 48, K = 6, Y = 22;                                        // the time
  int x0 = 100 - (4 * W + 3 * 6 + 14) / 2;
  digit(x0, Y, t.tm_hour / 10, W, H, K); digit(x0 + W + 6, Y, t.tm_hour % 10, W, H, K);
  int cx = x0 + 2 * W + 12; rect(cx + 1, Y + 14, 6, 6, true); rect(cx + 1, Y + H - 20, 6, 6, true);
  digit(cx + 14, Y, t.tm_min / 10, W, H, K); digit(cx + 14 + W + 6, Y, t.tm_min % 10, W, H, K);
  // v0.24.0: the weather sign is drawn first and trimmed to its own band. Before, the sun's rays ran into the date
  // and the rain and snow marks into the rule and the hours under it.
  wxIcon(wx.valid ? wx.code : 3, 26, 114, wx.valid ? wx.day : true);
  rect(0, 72, 64, 20, false); rect(0, 137, 64, 18, false);
  char d[24]; snprintf(d, sizeof d, L("%s %02d.%02d.", "%s %02d/%02d"), WD[t.tm_wday], t.tm_mday, t.tm_mon + 1);
  textLeft(EK_SMALL, 12, 74, d);                                                 // date left, alarm or timer right
  if (rtcTimerEnd > (uint32_t)time(NULL)) textRight(EK_SMALL, 188, 74, String((rtcTimerEnd - (uint32_t)time(NULL) + 59) / 60) + " min");
  else if (alOn) { char b[8]; snprintf(b, sizeof b, "%02d:%02d", alH, alM); bell(188 - textWidth(EK_SMALL, b) - 15, 76); textRight(EK_SMALL, 188, 74, b); }   // v0.24.0: the bell sat on the first digit
  textAt(EK_MID, 120, 98, deg(outsideC()) + L("  in ", "  int. ") + deg(insideC));
  textAt(EK_SMALL, 120, 122, rainLine());
  rect(8, 140, 184, 1, true);
  for (int i = 0; i < 6; i++) {                                                  // the next six hours
    int hx = 22 + i * 31; char b[8]; snprintf(b, sizeof b, "%02d", wx.valid ? wx.hh[i] : (t.tm_hour + 1 + i) % 24);
    textAt(EK_SMALL, hx, 143, b); textAt(EK_SMALL, hx, 160, wx.valid ? deg(wx.ht[i]) : "--");
    int wbar = wx.valid ? wx.hp[i] * 22 / 100 : 0; rect(hx - 11, 180, 22, 5, false); if (wbar) rect(hx - 11, 180, wbar, 5, true);
  }
}
#if CLIMATE
static int climPage = 0;
static int climPages() { return 4 + (wx.marine ? 1 : 0) + 1; }      // log, pressure, hours, days, [marine], air
static void climFace() {                       // the tile: now, the next two hours, today
  if (!wx.valid) {
    wxIcon(2, 100, 62, true); textAt(EK_MID, 100, 100, "Clim@te");
    textAt(EK_SMALL, 100, 136, String(L("in ", "int. ")) + deg(insideC) + "  " + (isnan(insideRH) ? String("--") : String((int)lroundf(insideRH))) + "%");
    if (!wifiConfigured()) {                   // v0.24.0: the face says it before any tap
      textAt(EK_SMALL, 100, 156, L("Wi-Fi not set up", "Wi-Fi non configur\xC3\xA9"));
      textAt(EK_SMALL, 100, 174, L("hold: set it up", "maintenir\xC2\xA0" ": configurer"));
    } else if (rtcWxMiss) {                    // asked for, and it did not come
      textAt(EK_SMALL, 100, 156, L("no connection", "pas de connexion"));
      textAt(EK_SMALL, 100, 174, L("tap to retry", "touchez\xC2\xA0" ": r\xC3\xA9" "essayer"));
    } else textAt(EK_SMALL, 100, 160, L("tap for the forecast", "touchez\xC2\xA0" ": pr\xC3\xA9" "visions"));
    return;
  }
  wxIcon(wx.code, 36, 42, wx.day);
  textAt(EK_BIG, 124, 22, deg(wx.t));
  textAt(EK_SMALL, 124, 60, String(compass(wx.wdir)) + " " + String(isnan(wx.wind) ? 0 : (int)lroundf(wx.wind)) + " km/h");
  int dir = 0; float p = presNow(), tr = presTrend(); zambretti(p, isnan(tr) ? 0 : tr, dir);
  textLeft(EK_SMALL, 12, 80, String(L("in ", "int. ")) + deg(insideC));
  textRight(EK_SMALL, 188, 80, isnan(p) ? String("---- hPa") : String((int)lroundf(p)) + " hPa " + (isnan(tr) ? "" : dir < 0 ? "\xE2\x86\x93" : dir > 0 ? "\xE2\x86\x91" : "\xE2\x86\x92"));
  textAt(EK_SMALL, 100, 96, rainLine());
  rect(8, 114, 184, 1, true);
  textLeft(EK_SMALL, 12, 118, "2h");
  for (int i = 0; i < 8; i++) { int h = (int)(wx.rain15[i] * 14); if (h > 14) h = 14; if (h) rect(44 + i * 18, 132 - h, 14, h, true); }
  rect(44, 132, 142, 1, true);
  rect(8, 140, 184, 1, true);
  textAt(EK_SMALL, 100, 144, String(L("today ", "aujourd\xE2\x80\x99" "hui ")) + deg(wx.dmin[0]) + " / " + deg(wx.dmax[0]));
  textAt(EK_SMALL, 100, 160, "\xE2\x86\x91 " + hhmm(wx.sunrise[0]) + "     \xE2\x86\x93 " + hhmm(wx.sunset[0]));
  time_t at = wx.at; struct tm ta; localtime_r(&at, &ta); char u[8]; snprintf(u, sizeof u, "%02d:%02d", ta.tm_hour, ta.tm_min);
  textAt(EK_SMALL, 100, 182, String(L("tap: more  \xC2\xB7  ", "centre\xC2\xA0" ": plus  \xC2\xB7  ")) + u);
}
static void pageHeader(const String &t, int p, int n) { textAt(EK_SMALL, 100, 1, t + "  " + String(p) + "/" + String(n)); rect(8, 16, 184, 1, true); }
static void pageFooter() { textAt(EK_SMALL, 100, 182, L("\xE2\x80\xB9 \xE2\x80\xBA pages  \xC2\xB7  hold: back", "\xE2\x80\xB9 \xE2\x80\xBA pages \xC2\xB7 maintenir")); }
static void presSeries(int16_t *out) {        // 96 hourly values: the model, with the sensor's log over the past 60 h
  memcpy(out, wx.pres, sizeof(int16_t) * 96);
  if (!pLocalFresh()) return;
  int n; BatSample *a = loadLog(n); if (!a) return;
  uint32_t now = time(NULL);
  for (int i = n - 1; i >= 0; i--) {
    if (!a[i].p10) continue;
    long back = (long)(now - a[i].t); if (back < 0 || back > 60L * 3600) continue;
    int k = 60 - (int)((back + 1800) / 3600); if (k >= 0 && k < 60) out[k] = a[i].p10;
  }
  free(a);
}
static void climPagePressure(int p, int n) {
  pageHeader(pLocalFresh() ? L("pressure", "pression") : L("pressure (model)", "pression (mod\xC3\xA8" "le)"), p, n);
  static int16_t ps[96]; presSeries(ps);
  float now = presNow(), tr = presTrend(); int dir = 0; int z = zambretti(now, isnan(tr) ? 0 : tr, dir);
  textLeft(EK_SMALL, 12, 22, isnan(now) ? String("---- hPa") : String((int)lroundf(now)) + " hPa");
  if (!isnan(tr)) textRight(EK_SMALL, 188, 22, String(dir < 0 ? "\xE2\x86\x93 " : dir > 0 ? "\xE2\x86\x91 " : "\xE2\x86\x92 ") + String(fabsf(tr), 1) + " / 3 h");
  int gx = 40, gy = 42, gw = 150, gh = 78; rect(gx, gy, 1, gh, true); rect(gx, gy + gh, gw, 1, true);
  float lo = 9999, hi = -9999; for (int i = 0; i < 96; i++) if (ps[i]) { float v = ps[i] / 10.0f; lo = fminf(lo, v); hi = fmaxf(hi, v); }
  if (lo > hi) { lo = 990; hi = 1030; } lo = floorf(lo / 5) * 5 - 5; hi = ceilf(hi / 5) * 5 + 5;
  textRight(EK_SMALL, 37, gy - 6, String((int)hi)); textRight(EK_SMALL, 37, gy + gh - 10, String((int)lo));
  for (int i = 0; i < 96; i++) { if (!ps[i]) continue; float v = ps[i] / 10.0f; int y = gy + gh - (int)((v - lo) * gh / (hi - lo)); int x = gx + i * gw / 96;
    if (i < 60) rect(x, y, 2, 2, true); else if (i % 2 == 0) disc(x, y, 1); }                 // 60 h back solid, 36 h ahead dotted
  rect(gx + 60 * gw / 96, gy, 1, gh, true); textAt(EK_SMALL, gx + 60 * gw / 96, gy + gh + 3, L("now", "maint."));
  if (z) { String v = EK_FR ? ZAMB_FR[z] : ZAMB_EN[z]; textBlock(v, 8, 136, 184, 44, false); }
  pageFooter();
}
static void climPageHours(int p, int n) {
  pageHeader(L("next 12 h", "12 prochaines h"), p, n);
  textLeft(EK_SMALL, 14, 20, "h"); textLeft(EK_SMALL, 40, 20, L("rain", "pluie")); textRight(EK_SMALL, 118, 20, "\xC2\xB0" "C"); textRight(EK_SMALL, 186, 20, "km/h");
  for (int i = 0; i < 12; i++) { int y = 36 + i * 12; char b[8]; snprintf(b, sizeof b, "%02d", wx.hh[i]); textLeft(EK_SMALL, 14, y, b);
    int w = wx.hp[i] * 44 / 100; rect(40, y + 4, 44, 6, false); if (w) rect(40, y + 4, w, 6, true);
    textRight(EK_SMALL, 118, y, deg(wx.ht[i])); textRight(EK_SMALL, 186, y, String(wx.hwind[i])); }
  pageFooter();
}
static const char *codeWord(int c) {           // WMO weather code -> one short word
  if (c == 0) return L("clear", "d\xC3\xA9" "gag\xC3\xA9"); if (c <= 2) return L("fair", "voil\xC3\xA9");
  if (c == 3) return L("cloudy", "couvert"); if (c <= 48) return L("fog", "brouillard");
  if (c <= 57) return L("drizzle", "bruine"); if (c <= 67) return L("rain", "pluie");
  if (c <= 77) return L("snow", "neige"); if (c <= 82) return L("showers", "averses");
  if (c <= 86) return L("snow", "neige"); return L("storm", "orage");
}
static void climPageDays(int p, int n) {
  pageHeader(L("5 days", "5 jours"), p, n);
  time_t e = time(NULL); struct tm t; localtime_r(&e, &t);
  for (int i = 0; i < 5; i++) { int y = 22 + i * 31; int wd = (t.tm_wday + i) % 7;     // two lines per day
    textLeft(EK_SMALL, 12, y, i == 0 ? L("today", "auj.") : WD[wd]);
    textRight(EK_SMALL, 188, y, codeWord(wx.dcode[i]));
    textLeft(EK_SMALL, 24, y + 14, deg(wx.dmin[i]) + " / " + deg(wx.dmax[i]));
    textRight(EK_SMALL, 188, y + 14, wx.dprec[i] >= 0.1f ? String(wx.dprec[i], wx.dprec[i] < 10 ? 1 : 0) + " mm" : String("")); }
  pageFooter();
}
static void climPageMarine(int p, int n) {
  pageHeader(L("marine", "mer"), p, n);
  textAt(EK_SMALL, 100, 22, String(L("waves ", "houle ")) + String(wx.wave, 1) + " m \xC2\xB7 " + String(isnan(wx.wperiod) ? 0 : (int)lroundf(wx.wperiod)) + " s \xC2\xB7 " + compass(wx.wavedir));
  int gx = 12, gy = 42, gw = 176, gh = 60; rect(gx, gy + gh / 2, gw, 1, true);
  int lo = 32767, hi = -32768; for (int i = 0; i < 192; i++) if (wx.sea[i] != INT16_MIN) { lo = min(lo, (int)wx.sea[i]); hi = max(hi, (int)wx.sea[i]); }
  if (hi > lo) for (int i = 0; i < 192; i++) { if (wx.sea[i] == INT16_MIN) continue; int y = gy + gh - (int)((wx.sea[i] - lo) * (gh - 2) / (hi - lo)) - 1; rect(gx + i * gw / 192, y, 1, 2, true); }
  int ni = (int)((nowS() - wx.seaT0) / 900); if (ni >= 0 && ni < 192) { rect(gx + ni * gw / 192, gy, 1, gh, true); textAt(EK_SMALL, max(gx + 14, gx + ni * gw / 192), gy + gh + 2, L("now", "maint.")); }
  int m; float h;
  if (tideNext(true, m, h)) { time_t at = time(NULL) + m * 60; struct tm ta; localtime_r(&at, &ta); char b[8]; snprintf(b, sizeof b, "%02d:%02d", ta.tm_hour, ta.tm_min);
    textLeft(EK_SMALL, 12, 122, String(L("high ", "PM ")) + b); textRight(EK_SMALL, 188, 122, String(h, 1) + " m"); }
  if (tideNext(false, m, h)) { time_t at = time(NULL) + m * 60; struct tm ta; localtime_r(&at, &ta); char b[8]; snprintf(b, sizeof b, "%02d:%02d", ta.tm_hour, ta.tm_min);
    textLeft(EK_SMALL, 12, 138, String(L("low ", "BM ")) + b); textRight(EK_SMALL, 188, 138, String(h, 1) + " m"); }
  textAt(EK_SMALL, 100, 158, String(L("sea ", "mer ")) + deg(wx.sst) + L("  \xC2\xB7  indicative", "  \xC2\xB7  indicatif"));
  pageFooter();
}
static void climPageAir(int p, int n) {
  pageHeader(L("air", "air"), p, n);
  if (!wx.air) { textBlock(L("no air data", "pas de donn\xC3\xA9" "es air"), 8, 60, 184, 80, false); pageFooter(); return; }
  textAt(EK_BIG, 44, 24, String(wx.aqi));
  const char *lab = wx.aqi <= 20 ? L("good", "bon") : wx.aqi <= 40 ? L("fair", "moyen") : wx.aqi <= 60 ? L("moderate", "d\xC3\xA9" "grad\xC3\xA9") : wx.aqi <= 80 ? L("poor", "mauvais") : L("very poor", "tr\xC3\xA8" "s mauvais");
  textLeft(EK_SMALL, 80, 24, L("EU index", "indice UE"));
  if (textWidth2(EK_MID, lab) <= 116) textLeft(EK_MID, 80, 40, lab); else textLeft(EK_SMALL, 80, 44, lab);   // "très mauvais" is too wide for the large type
  rect(8, 66, 184, 1, true);
  textLeft(EK_SMALL, 12, 70, "PM2.5  " + String(isnan(wx.pm25) ? 0 : (int)lroundf(wx.pm25))); textRight(EK_SMALL, 188, 70, "PM10  " + String(isnan(wx.pm10) ? 0 : (int)lroundf(wx.pm10)));
  textLeft(EK_SMALL, 12, 86, "O\xE2\x82\x83  " + String(isnan(wx.o3) ? 0 : (int)lroundf(wx.o3)));       textRight(EK_SMALL, 188, 86, "NO\xE2\x82\x82  " + String(isnan(wx.no2) ? 0 : (int)lroundf(wx.no2)));
  textLeft(EK_SMALL, 12, 102, "\xC2\xB5g/m\xC2\xB3   \xC2\xB7   pollen 0-3");
  rect(8, 118, 184, 1, true);
  const char *P[6] = { L("alder", "aulne"), L("birch", "bouleau"), L("grass", "gramin\xC3\xA9" "es"), L("mugwort", "armoise"), L("olive", "olivier"), L("ragweed", "ambroisie") };
  // v0.24.0: herbs on the left, trees on the right. In the order of the data, "graminées" ran into the next column and
  // "ambroisie" off the paper: the two widest words now share the wider column.
  static const uint8_t ORD[6] = { 2, 0, 5, 1, 3, 4 };       // grass | alder, ragweed | birch, mugwort | olive
  for (int r = 0; r < 6; r++) { int i = ORD[r], x = (r % 2) ? 113 : 2, y = 122 + (r / 2) * 16; for (int k = 0; k < 3; k++) rect(x + k * 7, y + 4, 5, 8, k < wx.pollen[i]); textLeft(EK_SMALL, x + 23, y, P[i]); }
  pageFooter();
}
static void climDraw() {
  epd->EPD_Clear(); int n = climPages(), p = climPage;
  if (p == 0) { drawClimate(); textAt(EK_SMALL, 100, 1, String(L("7 days  1/", "7 jours  1/")) + String(n)); }
  else if (p == 1) climPagePressure(p + 1, n);
  else if (p == 2) climPageHours(p + 1, n);
  else if (p == 3) climPageDays(p + 1, n);
  else if (p == 4 && wx.marine) climPageMarine(p + 1, n);
  else climPageAir(p + 1, n);
  show();
}
static void climEnter() {
  if (!wx.valid) {
    if (!wifiConfigured()) return;             // nothing to fetch with: the face already says so, and the paper is left alone
    epd->EPD_Clear(); tileDots(); textBlock(L("forecast...", "pr\xC3\xA9" "visions..."), 8, 60, 184, 80, false); show();
    rtcWxMiss = !wxFetchAll(); screenIdle(); return;
  }
  climOn = true; climPage = 0; climDraw();
}
static void climRefresh() {                    // middle tap inside the pages
  epd->EPD_Clear(); textBlock(L("updating...", "mise \xC3\xA0" " jour..."), 8, 60, 184, 80, false); show();
  wxFetchAll(); climDraw();
}
#endif


// ---- Wi-Fi and clock setup on the device (hotspot + page), in every edition since v0.25.0.
// No credentials in the firmware: the owner adds networks from a phone, the device stores them (up to 5).
#include <WebServer.h>
#include <DNSServer.h>
static bool wifiConfigured() { splitNets(); return nets > 0; }
static void drawQr(const char *text, int cy, int box) {
  esp_qrcode_config_t cfg = { qrGrab, 10, ESP_QRCODE_ECC_MED }; qrSize = 0;
  if (esp_qrcode_generate(&cfg, text) != ESP_OK || !qrSize) return;
  int sc = max(1, min(5, box / (qrSize + 2))), side = qrSize * sc, x0 = 100 - side / 2, y0 = cy - side / 2;
  for (int r = 0; r < qrSize; r++) for (int c = 0; c < qrSize; c++) if (qrM[r * qrSize + c]) rect(x0 + c * sc, y0 + r * sc, sc, sc, true);
}
#if EK_INVISIBLEOCEAN
#define COMPANION_URL "https://einkster-bible.netlify.app/"   // the bible, until it has a page for this edition
static void drawBox() {                      // stays on the paper, unpowered, until the owner starts
  epd->EPD_Clear();
  textAt(EK_MID, 100, 2, "E!NKSTER 154");
  textAt(EK_SMALL, 100, 28, "Édition InvisibleOcean");
  drawQr(COMPANION_URL, 96, 108);                     // 3 px per module: easy to scan
  textAt(EK_MID, 100, 148, "Scannez-moi");
  textAt(EK_SMALL, 100, 180, "Wi-Fi" "\xC2\xA0" ": touchez");
}
static void unbox() { boxMode = false; prefs.putBool("unboxed", true); tile = TILES[0]; prefs.putInt("tile", tile); }
#endif
static String htmlEsc(const String &s) {
  String o; for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c; }
  return o;
}
// The hotspot's password: 8 characters, made once per device, kept in the settings. No 0/o, 1/l/i.
static String apPass() {
  String pw = prefs.getString(EK_SET_HOTSPOT_PW, "");
  if (pw.length() == 8) return pw;
  static const char AB[] = "abcdefghjkmnpqrstuvwxyz23456789";
  pw = ""; for (int i = 0; i < 8; i++) pw += AB[esp_random() % (sizeof AB - 1)];
  prefs.putString(EK_SET_HOTSPOT_PW, pw);
  return pw;
}
static String apName() { uint8_t mac[6]; WiFi.macAddress(mac); char ap[24]; snprintf(ap, sizeof ap, "E!nkster-%02X%02X", mac[4], mac[5]); return String(ap); }
// The phone says its zone by name (Europe/Paris). Known names get their rule with summer time;
// any other zone gets a fixed offset from the phone, with no summer time.
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
  if (offMin < -14 * 60 || offMin > 12 * 60) return tzRule();                 // nonsense: keep what we have
  char b[24]; int a = abs(offMin);                                            // the phone's offset, POSIX sign: minutes behind world time
  if (a % 60) snprintf(b, sizeof b, "LT%c%d:%02d", offMin < 0 ? '-' : '+', a / 60, a % 60);
  else snprintf(b, sizeof b, "LT%c%d", offMin < 0 ? '-' : '+', a / 60);
  return String(b);
}
// ---- the device tile: fixed, last in the ring, in every edition. Hold: the Wi-Fi and clock page.
static bool layout8() { return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL) != NULL; }
static void deviceFace() {
  textAt(EK_MID, 100, 14, L("device", "appareil"));
  textAt(EK_SMALL, 100, 42, apName() + "  " EK_VERSION);
  splitNets();
  String w = !nets ? String(L("Wi-Fi not set up", "Wi-Fi non configur\xC3\xA9"))
           : nets == 1 ? String(L("Wi-Fi: 1 network", "Wi-Fi\xC2\xA0" ": 1 r\xC3\xA9" "seau"))
           : String(L("Wi-Fi: ", "Wi-Fi\xC2\xA0" ": ")) + nets + L(" networks", " r\xC3\xA9" "seaux");
  textAt(EK_SMALL, 100, 59, w);
  if (timeValid()) {
    time_t e = time(NULL); struct tm t; localtime_r(&e, &t); char b[24];
    strftime(b, sizeof b, "%d.%m.%Y  %H:%M", &t); textAt(EK_SMALL, 100, 76, b);
  } else textAt(EK_SMALL, 100, 76, L("clock not set", "heure non r\xC3\xA9" "gl\xC3\xA9" "e"));
  if (!layout8())                                     // an update file on a unit that never had the full flash of v0.24.1 or later
    textAt(EK_SMALL, 100, 97, L("old layout: one full flash", "flash complet requis"));
  else textAt(EK_SMALL, 100, 97, L("tap: sensors", "touchez\xC2\xA0" ": capteurs"));   // v0.25.3; v0.25.5: the top half
  rect(8, NL_Y, 184, 1, true);
  // v0.25.5: the bottom half is the lamp switch, drawn as one: off, the knob left; on, filled, the knob right
  const int sx = 14, sy = 132; bool on = rtcNlOn || rtcBmpWire == 2;
  rect(sx, sy, 44, 2, true); rect(sx, sy + 20, 44, 2, true); rect(sx, sy, 2, 22, true); rect(sx + 42, sy, 2, 22, true);
  if (on) { rect(sx + 2, sy + 2, 40, 18, true); rect(sx + 26, sy + 5, 12, 12, false); }
  else rect(sx + 6, sy + 5, 12, 12, true);
  textLeft(EK_SMALL, 68, 126, L("night light", "veilleuse"));
  textLeft(EK_SMALL, 68, 144, rtcBmpWire == 2 ? L("fixed (3V3)", "fixe (3V3)") : rtcNlOn ? L("on, 15 min", "allum\xC3\xA9" "e, 15 min") : L("off", "\xC3\xA9" "teinte"));
  textAt(EK_SMALL, 100, 178, L("hold: Wi-Fi and clock", "maintenir\xC2\xA0" ": configurer"));
}
// ---- v0.25.3: the Sensors page. The chips that answer on the I2C bus the header shares, then the pressure sensor live:
// hPa where the board is (one decimal), the same at sea level, its own temperature, and the change since the last zero in
// pascals with the height it stands for (1 m is about 12 Pa). The online value stays apart, marked as the model's.
// Tap: zero (or look again when no sensor answers). Hold: back. It stays awake 3 minutes after the last touch.
static bool snOn = false;
static uint8_t snAddr[24]; static int snN = 0;
static float snZero = NAN, snLast = NAN, snLastT = NAN, snLo = 0, snHi = 0;
static int snNote = 0;                         // v0.26.0, the height tap: 0 none yet, 1 no online reading, 2 out of range, 3 set
#define SN_MATCH_Y 157                         // v0.26.0: a tap below this line sets the height
static uint32_t snOpenAt = 0, snReadAt = 0; static int snReads = 0;
#define SN_AWAKE_MS (3UL * 60 * 1000)
#define SN_EVERY_MS 2000
static void snLine(int y, const String &a, const String &b) { textAt(EK_SMALL, 100, y, textWidth(EK_SMALL, a) <= 190 ? a : b); }   // the long wording when it fits
static const char *snChip() { return bmpId == 0x60 ? "BMP390" : "BMP388"; }
static void snScan() {
  if (!bmpDev) bmpTried = false;              // plugged in since this wake began: look again
  bmpInit(); flSensor(bmpDev != NULL);
  bmpHold = bmpDev && bmpSwitched; if (bmpHold) bmpPower(true);   // v0.25.4: on while the page is open
  snN = 0;
  i2c_master_bus_handle_t bus = NULL;
  if (i2c_master_get_bus_handle(I2C_NUM_0, &bus) == ESP_OK && bus)
    for (int a = 0x08; a <= 0x77 && snN < 24; a++) if (i2c_master_probe(bus, a, 20) == ESP_OK) snAddr[snN++] = a;
}
// Four readings at the finest setting, averaged: about 0.2 s. A finger on the paper stops it, so the tap is not missed;
// then nothing changes and it returns false. The paper's own refresh (about 0.4 s) cannot listen: a tap then may need repeating.
static bool snRead() {
  snReadAt = millis();
  if (!bmpDev) { snLast = NAN; return true; }
  double sp = 0, st = 0; int k = 0;
  for (; k < 4; k++) {
    if (touched()) return false;
    float hp, tc; if (!bmpMeasure(0x0C, 45, 1, hp, tc)) break;
    sp += hp; st += tc;
  }
  snReads++;
  if (k < 4) { snLast = NAN; return true; }
  snLast = (float)(sp / 4); snLastT = (float)(st / 4);
  if (isnan(snZero)) { snZero = snLast; snLo = snHi = 0; }
  float d = (snLast - snZero) * 100; if (d < snLo) snLo = d; if (d > snHi) snHi = d;
  return true;
}
static void snDraw() {
  epd->EPD_Clear();
  textAt(EK_SMALL, 100, 1, bmpDev ? L("sensors  \xC2\xB7  tap: zero", "capteurs \xC2\xB7 touchez\xC2\xA0: z\xC3\xA9ro") : L("sensors", "capteurs"));
  rect(8, 16, 184, 1, true);
  String l1 = L("bus:", "bus\xC2\xA0:"), l2;
  if (!snN) l1 += L(" nothing answers", " aucune r\xC3\xA9" "ponse");
  for (int i = 0; i < snN; i++) {
    char b[5]; snprintf(b, sizeof b, " %02X", snAddr[i]);
    if (!l2.length() && textWidth(EK_SMALL, l1 + b) <= 184) l1 += b; else l2 += b;
  }
  textAt(EK_SMALL, 100, 20, l1); if (l2.length()) textAt(EK_SMALL, 100, 36, l2.substring(1));
  if (bmpDev) {                                // v0.25.4: and where its power comes from
    char b[8]; snprintf(b, sizeof b, "0x%02X", bmpAt); String pw = bmpSwitched ? "GP2" : "3V3";
    if (!l2.length()) {                        // the bus fits one line: the chip, then its power, on their own lines
      textAt(EK_SMALL, 100, 36, String(snChip()) + L(" found at ", " trouv\xC3\xA9 en ") + b);
      textAt(EK_SMALL, 100, 54, bmpSwitched ? L("power: GP2, switched", "alim.\xC2\xA0: GP2, commut\xC3\xA9" "e") : L("power: 3V3, always on", "alim.\xC2\xA0: 3V3, permanente"));
    } else textAt(EK_SMALL, 100, 54, String(snChip()) + " " + b + " \xC2\xB7 " + pw);
  }
  else textAt(EK_SMALL, 100, 54, L("BMP388 not found", "BMP388 introuvable"));
  rect(8, 73, 184, 1, true);
  if (bmpDev && !isnan(snLast)) {
    String tc = String(snLastT, 1) + "\xC2\xB0" "C";
    snLine(77, String(L("sensor, here  \xC2\xB7  ", "capteur, ici  \xC2\xB7  ")) + tc, String(L("sensor  ", "capteur  ")) + tc);
    textAt(EK_MID, 100, 94, String(snLast, 1) + " hPa");
    String sea = String(toSeaLevel(snLast, snLastT), 1);
    snLine(119, String(L("sea level ", "niveau mer ")) + sea + ", " + altM + " m", String(L("sea ", "mer ")) + sea + ", " + altM + " m");   // v0.26.0: with the height
    float dPa = (snLast - snZero) * 100, rho = snLast * 100 / (287.05f * (snLastT + 273.15f)), dm = -dPa / (rho * 9.81f);
    String c = String(L("change ", "\xC3\xA9" "cart ")) + String((int)lroundf(dPa)) + " Pa";
    snLine(137, c + ", " + (dm >= 0 ? "+" : "") + String(dm, 1) + " m", c);
  } else if (bmpDev) textBlock(L("the sensor did not answer this reading", "le capteur n\xE2\x80\x99" "a pas r\xC3\xA9" "pondu \xC3\xA0" " cette lecture"), 8, 78, 184, 76, false);
  else textBlock(L("check the SDA and SCL wires, then tap to retry", "v\xC3\xA9" "rifiez les fils SDA et SCL, puis touchez pour relire"), 8, 78, 184, 76, false);
  rect(8, 157, 184, 1, true);
  bool online = wx.valid && !isnan(wx.pmsl) && wx.pmsl > 0;
  String hp = online ? String((int)lroundf(wx.pmsl)) + " hPa" : String("--");
  snLine(161, String(L("online, ", "en ligne, ")) + EK_WX_TOWN + ": " + hp, String(EK_WX_TOWN) + ": " + hp);   // v0.26.0: the town it is for
  if (bmpDev && snNote == 3) snLine(181, String(L("height set: ", "hauteur r\xC3\xA9gl\xC3\xA9" "e\xC2\xA0: ")) + altM + " m", String(altM) + " m");
  else if (bmpDev && snNote == 1) snLine(181, L("no online reading yet", "pas encore de mesure en ligne"), L("no online reading", "pas de mesure"));
  else if (bmpDev && snNote == 2) snLine(181, L("height out of range: not set", "hauteur hors limites"), L("out of range", "hors limites"));
  else if (bmpDev) snLine(181, String(L("tap here: match ", "touchez ici\xC2\xA0: caler sur ")) + EK_WX_TOWN, String(L("match ", "caler\xC2\xA0: ")) + EK_WX_TOWN);
  else snLine(181, L("tap: retry  \xC2\xB7  hold: back", "touchez\xC2\xA0: relire \xC2\xB7 maintenir\xC2\xA0: retour"),
                   L("tap: retry \xC2\xB7 hold: back", "relire \xC2\xB7 maintenir\xC2\xA0: retour"));
  show();
}
static void snEnter() {
  epd->EPD_Clear(); textBlock(L("looking at the bus...", "lecture du bus..."), 8, 60, 184, 80, false); show();
  snScan(); snZero = NAN; snLast = NAN; snReads = 0; snOpenAt = millis(); snNote = 0; snRead();
  String a; for (int i = 0; i < snN; i++) { char b[5]; snprintf(b, sizeof b, " %02X", snAddr[i]); a += b; }
  char m[64]; if (bmpDev) snprintf(m, sizeof m, "%s at 0x%02X, %s", snChip(), bmpAt, bmpSwitched ? "power switched on GP2, released when off" : "power always on (3V3)"); else snprintf(m, sizeof m, "no BMP388");
  flEvent("sensor", String("check: ") + snN + " on the bus (hex" + a + "), " + m);
  snOn = true; snDraw(); lastTouch = millis();
}
static void snExit() {
  snOn = false;
  if (bmpHold) { bmpHold = false; if (!rtcNlOn) bmpPower(false); }   // v0.25.4; v0.25.5: not while the night light is on
  String d = String("sensors page closed after ") + (int)((millis() - snOpenAt) / 1000) + " s, " + snReads + " readings";
  if (bmpDev && !isnan(snZero)) d += String(", change since zero from ") + (int)lroundf(snLo) + " to " + (int)lroundf(snHi) + " Pa";
  flEvent("sensor", d);
  lastTouch = millis(); screenIdle();
}
// v0.26.0: the height that makes the sensor's sea-level value equal the online one, from the barometric formula
// toSeaLevel() uses, solved for the height: r = (p / msl)^(1 / 5.257), h = (1 - r)(T + 273.15) / (0.0065 r).
// It is right only where the online reading is for the place the device stands (EK_WX_TOWN): contract v17.
static void snMatch() {
  if (!bmpDev || isnan(snLast)) return;
  if (!wx.valid || isnan(wx.pmsl) || wx.pmsl < 900 || wx.pmsl > 1100 || nowS() - wx.at > 3 * 3600) { snNote = 1; return; }
  float r = powf(snLast / wx.pmsl, 1.0f / 5.257f), h = (1 - r) * (snLastT + 273.15f) / (0.0065f * r);
  if (isnan(h) || h < -50 || h > 1500) { snNote = 2; return; }
  altM = (int)lroundf(h); prefs.putInt(EK_SET_ALT, altM); snNote = 3;
  memset(rtcPhT, 0, sizeof rtcPhT);            // the sensor's own 3-hour history was at the old height: the trend starts again
  wx.pLocal = 0;                               // and the barometer waits for the next reading at the new height
  flEvent("sensor", "height set to match the online reading");   // the height itself is personal: never logged
}
static void snLoop() {
  if (millis() - lastTouch > SN_AWAKE_MS) { snExit(); return; }
  if (touched()) {
    int x = 0, y = 0; touchXY(x, y); uint32_t t0 = millis(); bool hold = false;
    while (touched()) { if (millis() - t0 >= 600) { hold = true; break; } delay(12); }
    waitRelease(); lastTouch = millis();
    if (hold) { snExit(); return; }
    if (bmpDev && !isnan(snLast) && y >= SN_MATCH_Y) { snMatch(); snDraw(); return; }   // v0.26.0: the bottom line sets the height
    if (bmpDev && !isnan(snLast)) { snZero = snLast; snLo = snHi = 0; } else snScan();   // tap: zero, or look again
    snRead(); snDraw(); return;
  }
  if (millis() - snReadAt >= SN_EVERY_MS) { if (snRead()) snDraw(); return; }
  delay(30);
}
static bool wrongPassword(int r) { return r == 2 || r == 15 || r == 202 || r == 204; }
static void wifiSetup() {
  WiFi.disconnect(true); WiFi.mode(WIFI_AP_STA); delay(100);
  screenText(L("Looking for Wi-Fi networks...", "Recherche des réseaux Wi-Fi..."), false);
  int n = WiFi.scanNetworks(false, false);
  String opts; int ok = 0;
  for (int i = 0; i < n; i++) {                                 // strongest first, no duplicates
    String name = WiFi.SSID(i); if (!name.length()) continue;
    bool dup = false; for (int j = 0; j < i; j++) if (WiFi.SSID(j) == name) dup = true;
    if (dup) continue;
    bool ent = WiFi.encryptionType(i) == WIFI_AUTH_WPA2_ENTERPRISE || name.equalsIgnoreCase("eduroam");
    if (ent) opts += "<option disabled>" + htmlEsc(name) + L(" (sign-in network: not supported)", " (identifiant d’établissement : non pris en charge)") + "</option>";
    else { opts += "<option value=\"" + htmlEsc(name) + "\">" + htmlEsc(name) + "</option>"; ok++; }
  }
  String ap = apName(), pw = apPass();
  WiFi.softAP(ap.c_str(), pw.c_str());
  DNSServer dns; dns.start(53, "*", WiFi.softAPIP());
  WebServer srv(80); String saved, note, prevPass; bool done = false, existed = false;
  const char *closedWhy = "5 minutes";                           // v0.25.1: the field log says why the page closed
  const char *css = "<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;font-size:19px;line-height:1.5;margin:0;padding:22px;"
    "background:#f3efe6;color:#1c1b19;max-width:560px}h1{font-size:25px;margin:0 0 12px}h2{font-size:21px;margin:28px 0 6px}"
    "label{display:block;margin:18px 0 6px;font-weight:600}"
    "select,input{font-size:19px;width:100%;padding:12px;border:2px solid #1c1b19;border-radius:8px;box-sizing:border-box;background:#fff}"
    "button{margin-top:22px;font-size:19px;font-weight:700;padding:14px;width:100%;border:0;border-radius:10px;background:#1c1b19;color:#f3efe6}"
    ".row{display:flex;gap:12px;align-items:center;border-bottom:1px solid #cfc8b8;padding:8px 0}.row span{flex:1}"
    ".row button{margin:0;width:auto;padding:8px 14px;font-size:16px;background:#6b665c}"
    ".n{color:#6b665c;font-size:16px}.ok{background:#e3dccb;padding:10px 14px;border-radius:8px}</style>";
  auto page = [&]() {
    String h = String("<!doctype html><html lang=") + L("en", "fr") + "><title>E!nkster · Wi-Fi</title>" + css + "<h1>E!nkster · Wi-Fi</h1>"
      "<p id=gone class=ok style='display:none;position:fixed;top:0;left:0;right:0;margin:0;border-radius:0;background:#1c1b19;color:#f3efe6;padding:16px 22px'>" +
      L("The setup page is closed: long-press the device tile again.", "La page de réglage est fermée&nbsp;: maintenez de nouveau la tuile appareil.") + "</p>";
    if (note.length()) h += "<p class=ok>" + note + "</p>";
    h += String("<h2>") + L("Saved networks", "Réseaux enregistrés") + "</h2>";
    if (!nets) h += String("<p class=n>") + L("None yet.", "Aucun pour l’instant.") + "</p>";
    for (int i = 0; i < nets; i++) {
      h += "<form method=post action=/del class=row><span>" + htmlEsc(wnets.name[i]) +
           (i == wnets.last ? String(" <small class=n>") + L("(last used)", "(dernier utilisé)") + "</small>" : String("")) +
           "</span><input type=hidden name=i value=" + String(i) + "><button>" + L("Remove", "Retirer") + "</button></form>";
    }
    h += String("<h2>") + L("Add a network", "Ajouter un réseau") + "</h2>"
      "<form method=post action=/save><label>" + L("Network", "Réseau") + "</label><select name=s>" +
      (ok ? opts : String("<option disabled>") + L("No network found", "Aucun réseau trouvé") + "</option>") +
      "<option value=\"\">" + L("Another network (type its name below)", "Autre réseau (tapez son nom ci-dessous)") + "</option>"
      "</select><label>" + L("Name of another network", "Nom d’un autre réseau") + "</label><input name=h autocomplete=off>"
      "<label>" + L("Password", "Mot de passe") + "</label><input name=p type=password autocomplete=off>"
      "<button>" + L("Save and connect", "Enregistrer et connecter") + "</button></form>"
      "<p class=n>" + L("Up to 5 networks. A network that is not here today can be saved for later: the device keeps it.",
                        "Jusqu’à 5 réseaux. Un réseau absent aujourd’hui peut être enregistré pour plus tard&nbsp;: l’appareil le garde.") + "</p>"
      "<p class=n>" + L("Sign-in networks (eduroam and others) are not supported: use a home Wi-Fi or your phone's hotspot.",
                        "Les réseaux d’établissement (eduroam et autres réseaux à identifiant) ne sont pas pris en charge&nbsp;: utilisez le Wi-Fi de la maison ou le partage de connexion de votre téléphone.") + "</p>"
      "<h2>" + L("Clock", "Horloge") + "</h2><form method=post action=/time id=tf>"
      "<input type=hidden name=e><input type=hidden name=z><input type=hidden name=o>"
      "<button type=button onclick=\"var f=document.getElementById('tf');f.e.value=Math.floor(Date.now()/1000);"
      "try{f.z.value=Intl.DateTimeFormat().resolvedOptions().timeZone||''}catch(x){}f.o.value=new Date().getTimezoneOffset();f.submit()\">" +
      L("Set the clock from this phone", "Régler l’horloge sur ce téléphone") + "</button></form>"
      "<p class=n>" + L("The device then keeps the time without Wi-Fi, and changes to summer or winter time by itself.",
                        "L’appareil garde ensuite l’heure sans Wi-Fi, et passe seul à l’heure d’été ou d’hiver.") + "</p>";
    char tv[12]; snprintf(tv, sizeof tv, "%.1f", prefs.getInt(EK_SET_TEMP_OFF, EK_TEMP_OFF_DEFAULT) / 10.0f);   // v0.25.1
    h += String("<h2>") + L("Room temperature", "Température de la pièce") + "</h2><form method=post action=/toff>"
      "<label>" + L("Correction in °C", "Correction en °C") + "</label><input name=t type=number step=0.1 min=-5 max=5 value=" + tv + ">"
      "<button>" + L("Save the correction", "Enregistrer la correction") + "</button></form>"
      "<p class=n>" + L("The case warms the sensor a little. Put a thermometer next to the device for an hour, then enter the difference: -1.0 if the device reads one degree too high.",
                        "Le boîtier réchauffe un peu le capteur. Posez un thermomètre à côté de l’appareil pendant une heure, puis entrez l’écart&nbsp;: -1,0 si l’appareil indique un degré de trop.") + "</p>"
      "<h2>" + L("Language", "Langue") + "</h2><form method=post action=/lang class=row><span>" +   // v0.26.0
      L("The device and this page", "L\u2019appareil et cette page") + "</span>"
      "<button name=l value=en>English</button><button name=l value=fr>Fran\u00e7ais</button></form>"
      "<script>var f=0;setInterval(function(){var c=new AbortController();setTimeout(function(){c.abort()},2500);"
      "fetch('/ping',{cache:'no-store',signal:c.signal}).then(function(r){f=r.ok?0:f+1}).catch(function(){f++})"
      ".then(function(){if(f>=2)document.getElementById('gone').style.display='block'})},4000)</script>";
    return h;
  };
  auto reply = [&]() { srv.send(200, "text/html; charset=utf-8", page()); };
  srv.on("/save", HTTP_POST, [&]() {
    String ss = srv.arg("s"), pp = srv.arg("p"), hh = srv.arg("h");
    hh.trim();
    if (hh.length()) ss = hh;                                       // v0.25.1: a typed name wins over the list
    ss.trim();
    EkPrefsKV kv(prefs);
    int had = ss.length() ? wnets.find(ss.c_str()) : -1;            // already stored: keep its password, in case the new one is wrong
    existed = had >= 0; prevPass = existed ? String(wnets.pass[had]) : String();
    if (ss.length() && wnets.add(kv, ss.c_str(), pp.c_str())) {
      saved = ss; done = true;
      FL("wifi", "a network was saved on the setup page");
      srv.send(200, "text/html; charset=utf-8", String("<!doctype html><html><title>E!nkster · Wi-Fi</title>") + css + "<h1>" +
               L("Saved", "Enregistré") + "</h1><p>" + L("Look at the device: it is testing the connection.",
               "Regardez l’écran de l’E!nkster&nbsp;: il teste la connexion.") + "</p>");
      return;
    }
    note = L("Not saved: name missing, or name or password too long.", "Non enregistré&nbsp;: nom manquant, ou nom ou mot de passe trop long.");
    reply();
  });
  srv.on("/del", HTTP_POST, [&]() {
    EkPrefsKV kv(prefs); int i = srv.arg("i").toInt();
    if (i >= 0 && i < nets) { note = htmlEsc(wnets.name[i]) + L(": removed.", "&nbsp;: retiré."); wnets.removeAt(kv, i); nets = wnets.count; rtcNet = -1; FL("wifi", "a network was removed on the setup page"); }
    reply();
  });
  srv.on("/time", HTTP_POST, [&]() {
    long e = srv.arg("e").toInt();
    if (e > 1735689600L && e < 2051222400L) {
      struct timeval tv = { (time_t)e, 0 }; settimeofday(&tv, NULL);
      systemToRtcChip();
      String rule = tzFromPhone(srv.arg("z"), srv.arg("o").toInt());
      if (rule != tzRule()) setTzRule(rule);
      time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt); char b[8]; strftime(b, sizeof b, "%H:%M", &lt);
      note = String(L("Clock set: ", "Horloge réglée&nbsp;: ")) + b;
      FL("time", "clock set from the phone");
      rect(0, 0, 200, 19, false);                                // v0.25.1: the paper shows it too, so no one leaves the page to check
      textAt(EK_SMALL, 100, 1, String(L("Clock set ", "Heure r\xC3\xA9gl\xC3\xA9" "e ")) + b);
      show();
    } else note = L("The phone sent no usable time.", "Le téléphone n’a pas envoyé d’heure utilisable.");
    reply();
  });
  srv.on("/lang", HTTP_POST, [&]() {                                  // v0.26.0: the interface language
    String l = srv.arg("l");
    if (l == "en" || l == "fr") {
      prefs.putString(EK_SET_LANG, l); uiFr = l == "fr";
      note = L("Language saved: English.", "Langue enregistr\u00e9e\u00a0: fran\u00e7ais.");
      FL("setup", String("interface language set to ") + l);
    }
    reply();
  });
  srv.on("/ping", [&]() { srv.send(200, "text/plain", "ok"); });     // v0.25.1: the page asks every 4 s whether the device still answers
  srv.on("/toff", HTTP_POST, [&]() {                                  // v0.25.1: the temperature correction
    String a = srv.arg("t"); a.replace(",", ".");
    float v = a.toFloat();
    if (a.length() && v >= -5.0f && v <= 5.0f) {
      int t10 = lroundf(v * 10); char b[48];
      prefs.putInt(EK_SET_TEMP_OFF, t10);
      snprintf(b, sizeof b, "%+.1f", t10 / 10.0f);
      note = String(L("Temperature correction saved: ", "Correction enregistrée&nbsp;: ")) + b + " °C";
      FL("sensor", String("temperature correction set to ") + b);
    } else note = L("Correction not saved: give a value between -5 and +5.", "Correction non enregistrée&nbsp;: donnez une valeur entre -5 et +5.");
    reply();
  });
  srv.onNotFound([&]() { reply(); });                                   // any address: the page (captive)
  srv.begin();
  epd->EPD_Clear();                                              // the setup screen: join by QR, then open the address
  textAt(EK_SMALL, 100, 1, L("Wi-Fi setup", "Configuration Wi-Fi"));
  String join = String("WIFI:T:WPA;S:") + ap + ";P:" + pw + ";;";
  drawQr(join.c_str(), 62, 96);                                 // 3 px a square: the password makes the code one size larger
  textAt(EK_SMALL, 100, 108, String("1. Wi-Fi ") + ap);
  textAt(EK_SMALL, 100, 126, String(L("password ", "mot de passe ")) + pw);
  textAt(EK_SMALL, 100, 144, L("2. open 192.168.4.1", "2. ouvrez 192.168.4.1"));
  textAt(EK_SMALL, 100, 162, L("in your browser", "dans votre navigateur"));
  textAt(EK_SMALL, 100, 182, L("hold 2 s: cancel", "maintenir 2 s\xC2\xA0" ": annuler"));
  show();
  uint32_t t0 = millis(), hold0 = 0;
  FL("wifi", "setup page open");
  while (!done && millis() - t0 < 5UL * 60 * 1000) {            // 5 minutes to complete it
    dns.processNextRequest(); srv.handleClient();
    // v0.25.1: touches in the first 3 s are ignored (the long press that opened the page), and cancelling takes a 2 s hold
    if (millis() - t0 > 3000 && touched()) {
      if (!hold0) hold0 = millis(); else if (millis() - hold0 > 2000) { closedWhy = "a 2 s hold"; break; }
    } else hold0 = 0;
    if (digitalRead(PWR_BTN) == LOW) { closedWhy = "the side button"; break; }
    delay(2);
  }
  if (done) closedWhy = "a network saved";
  FL("wifi", String("setup page closed: ") + closedWhy);
  uint32_t t1 = millis(); while (done && millis() - t1 < 1500) { srv.handleClient(); delay(5); }   // let the reply go out
  srv.stop(); dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA);
  waitRelease();
  if (!done) { lastTouch = millis(); screenIdle(); return; }
  screenText(String(L("Connecting to ", "Connexion à ")) + saved + "...", false);
  int k = wnets.find(saved.c_str());
  netsChanged(); rtcNet = k; rtcNetDownUntil = 0; for (int i = 0; i < MAX_NETS; i++) netReason[i] = 0;
  wifiBegin(k);                                                  // the new network first
  bool up = wifiUp() && tryNet == k;
  if (up) {
    maybeSyncTime();
#if EK_INVISIBLEOCEAN
    if (boxMode) unbox();
#endif
    screenText(String(L("Connected to ", "Connecté à ")) + saved + L(".\n\nWeather and radio are ready.", ".\n\nLa météo et FIP sont prêtes."), false);
  } else {
    int r = netReason[k];
    String why = reasonText(r);
    if (wrongPassword(r)) {                                      // v0.25.0: a wrong password is not kept
      EkPrefsKV kv(prefs);
      if (existed) { wnets.add(kv, saved.c_str(), prevPass.c_str()); why += L("\nThe old password is kept.", "\nL’ancien mot de passe est gardé."); }
      else { wnets.removeAt(kv, k); why += L("\nNot kept.", "\nNon gardé."); }
      netsChanged();
    } else why += L("\nKept for later.", "\nGardé pour plus tard.");
    screenText(String(L("No connection to ", "Pas de connexion à ")) + saved + L(":\n", "\xC2\xA0" ":\n") + why +
               L("\n\nHold the device tile to try again.", "\n\nMaintenez la tuile appareil pour recommencer."), false);
  }
  delay(4000); lastTouch = millis(); screenIdle();
}

// ---- the Kaffeesatz tile: the paper's daily edito (see BRIEF-kaffeesatz-einkster-bridge.md)
#ifndef EK_KS_CHANNEL
#define EK_KS_CHANNEL ""
#endif
#define KS_URL        "https://kaffeesatz.netlify.app/.netlify/functions/einkster?c="
#define KS_FROM_MIN   (6 * 60 + 30)            // morning window: 06:30 to 11:00, every 30 min
#define KS_TO_MIN     (11 * 60)
RTC_DATA_ATTR static uint32_t rtcKsTry;       // last fetch attempt
static String ksDate, ksLang, ksLead, ksBrief; static bool ksLoaded = false, ksOn = false, ksSmallWas = false;
static volatile bool ksQuit = false;
static const char *MO[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static String todayKey() {
  time_t e = time(NULL); struct tm t; localtime_r(&e, &t); char b[12];
  snprintf(b, sizeof b, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday); return String(b);
}
static String ksNiceDate(const String &d) {           // "2026-09-29" -> "Tue 29 Sep"
  if (d.length() != 10) return d;
  struct tm t = {}; t.tm_year = d.substring(0, 4).toInt() - 1900; t.tm_mon = d.substring(5, 7).toInt() - 1;
  t.tm_mday = d.substring(8, 10).toInt(); t.tm_hour = 12; mktime(&t);
  return String(WD[t.tm_wday]) + " " + String(t.tm_mday) + " " + MO[t.tm_mon % 12];
}
static void ksParse(const String &b) {
  ksDate = jsonField(b, "date"); ksLang = jsonField(b, "lang");
  ksLead = jsonField(b, "lead"); ksBrief = jsonField(b, "brief");
}
static void ksLoad() {                                   // the cached edito, from flash
  if (ksLoaded) return; ksLoaded = true;
  if (!lfsMount()) return;
  File f = LittleFS.open("/ks.json", "r"); if (!f) return;
  String b = f.readString(); f.close(); ksParse(b);
}
static bool ksSetUp() { return strlen(EK_KS_CHANNEL) >= 32; }
static bool ksFresh() { ksLoad(); return timeValid() && ksDate == todayKey(); }
static int nowMin() { time_t e = time(NULL); struct tm t; localtime_r(&e, &t); return t.tm_hour * 60 + t.tm_min; }
// a morning timer wake: set up, not yet today's, inside the window, not tried in the last 25 min
static bool ksMorningDue() {
  if (!ksSetUp() || !timeValid() || ksFresh()) return false;
  int m = nowMin(); if (m < KS_FROM_MIN || m >= KS_TO_MIN) return false;
  return time(NULL) - (time_t)rtcKsTry >= 25 * 60;
}
static uint64_t ksWakeUs() {                            // next morning check
  if (!ksSetUp() || !timeValid()) return 0;
  int m = nowMin(); int secs;
  if (!ksFresh() && m >= KS_FROM_MIN && m < KS_TO_MIN) secs = 30 * 60;
  else secs = ((m < KS_FROM_MIN ? KS_FROM_MIN - m : 24 * 60 - m + KS_FROM_MIN) * 60);
  return (uint64_t)max(60, secs) * 1000000ULL;
}
static bool ksFetch() {                                 // true when an edito came in
  if (!ksSetUp()) return false;
  rtcKsTry = time(NULL);
  if (offlineNow()) return false;
  if (!wifiUp()) { rtcNetDownUntil = nowS() + NET_BACKOFF_S; return false; }
  String body; ksQuit = false;
  bool ok = FipStream::httpsGet(String(KS_URL) + EK_KS_CHANNEL,
    [&](const uint8_t *b, int n) { if (body.length() < 8192) body.concat((const char *)b, n); }, ksQuit);
  FL("fetch", String("kaffeesatz") + (ok ? " ok, " : " FAILED, ") + String(body.length()) + " bytes");
  if (!ok || jsonField(body, "brief").length() == 0) return false;
  ksParse(body); ksLoaded = true;
  if (lfsMount()) { File f = LittleFS.open("/ks.json", "w"); if (f) { f.print(body); f.close(); } }
  return true;
}
static void ksFace() {                                  // the tile: a small front page
  ksLoad();
  epd->EPD_Clear(); tileDots();
  textAt(EK_MID, 100, 18, "Kaffeesatz");
  rect(8, 44, 184, 2, true); rect(8, 48, 184, 1, true);
  if (!ksSetUp()) {
    textBlock("Not set up yet. The build needs the paper's channel code.", 8, 60, 184, 100, false);
  } else if (!ksBrief.length()) {
    textBlock("No edito yet. It arrives after you open Kaffeesatz in the morning.", 8, 60, 184, 100, false);
    textAt(EK_SMALL, 100, 178, "tap to check now");
  } else {
    textAt(EK_SMALL, 100, 52, ksNiceDate(ksDate));
    rect(8, 72, 184, 1, true);
    String lead = ksLead.length() ? ksLead : ksBrief.substring(0, min(160, (int)ksBrief.length()));
    textBlock(lead, 8, 80, 184, 88, true);
    textAt(EK_SMALL, 100, 178, ksFresh() ? String("tap to read") : String("from ") + ksNiceDate(ksDate) + "  \xC2\xB7  tap");
  }
  show(); onIdle = true;
}
static void ksDraw() {
  epd->EPD_Clear();
  rdHeader(ksNiceDate(ksDate), pgN > 1 ? String(rdPage + 1) + "/" + String(pgN) : String(""));
  drawBodyPage(-1);
  rdFooter(rdPage + 1 < pgN ? "more \xE2\x80\xBA" : "hold: back");
  show();
}
static void ksOpen() {
  docTitle = "From the editor";
  docBody = (ksLead.length() ? "**" + ksLead + "**\n\n" : String("")) + ksBrief;
  ksSmallWas = smallText; smallText = true;     // newspaper text: the compact size, while reading
  layoutDoc();
  rdPage = 0; ksOn = true; ksDraw();
}
static void ksExit() { ksOn = false; smallText = ksSmallWas; screenIdle(); }
static void ksTouch() {                                 // taps and swipes turn pages; hold goes back
  int x0 = 0, y0 = 0, x = 0, y = 0; touchXY(x0, y0); x = x0; y = y0;
  uint32_t t0 = millis(); bool down = true, hold = false;
  while (down) {
    int nx, ny; down = touchXY(nx, ny); if (down) { x = nx; y = ny; }
    if (down && abs(x - x0) < SWIPE_PX && abs(y - y0) < SWIPE_PX && millis() - t0 >= 600) { hold = true; break; }
    delay(12);
  }
  waitRelease();
  if (hold) { ksExit(); return; }
  int dir;
  if (abs(x - x0) >= SWIPE_PX) dir = (x < x0) != (bool)TOUCH_FLIP_X ? 1 : -1;   // swipe left = next page
  else { int tx = TOUCH_FLIP_X ? 199 - x0 : x0; dir = tx < 67 ? -1 : 1; }
  if (dir > 0 && rdPage + 1 < pgN) rdPage++;
  else if (dir < 0 && rdPage > 0) rdPage--;
  else if (dir > 0) { ksExit(); return; }                // past the last page: back to the front
  ksDraw();
}

// ---- the clock's deep dive: alarm and timer pages
static bool clkOn = false; static int clkPage = 0;     // 0 alarm, 1 timer
static void tri(int cx, int y, bool up) {
  for (int i = 0; i < 9; i++) rect(cx - i, up ? y + i : y + 8 - i, 2 * i + 1, 1, true);
}
static void pill(int x, int y, int w, const String &s, bool filled) {
  rect(x, y, w, 20, true); if (!filled) rect(x + 2, y + 2, w - 4, 16, false);
  int tw = textWidth2(EK_SMALL, s); textLeft(EK_SMALL, x + (w - tw) / 2, y + 2, s, !filled);
}
static bool timerRunning() { return rtcTimerEnd > (uint32_t)time(NULL); }
// layout: header 0-34 (title, page dots, big pill), marks 38-50, digits 56-130, marks 134-146,
// chips 152-174, bottom line 178-198
#define PILL_X 118
#define PILL_W 78
#define DIG_Y  56
static void bigPair(int x, int v) { digit(x, DIG_Y, v / 10, 38, 74, 9); digit(x + 44, DIG_Y, v % 10, 38, 74, 9); }
static void clkDraw() {
  epd->EPD_Clear();
  for (int i = 0; i < 2; i++) { if (i == clkPage) disc(85 + i * 14, 16, 4); else ring(85 + i * 14, 16, 4, 1, false); }
  bool run = clkPage == 1 && timerRunning();
  { String hd = clkPage == 0 ? L("alarm", "r\xC3\xA9" "veil") : L("timer", "minuteur");   // v0.24.0: "minuteur" in the large type ran over the two page dots
    if (textWidth2(EK_MID, hd) <= 72) textLeft(EK_MID, 6, 5, hd); else textLeft(EK_SMALL, 4, 9, hd); }
  rect(PILL_X, 2, PILL_W, 30, true);                       // big header button
  String pl = clkPage == 0 ? (alOn ? L("on", "oui") : L("off", "non")) : (run ? L("stop", "arr\xC3\xAA" "t") : L("start", "lancer"));
  bool filled = clkPage == 0 ? alOn : run;
  if (!filled) rect(PILL_X + 2, 4, PILL_W - 4, 26, false);
  int tw = textWidth2(EK_MID, pl); textLeft(EK_MID, PILL_X + (PILL_W - tw) / 2, 5, pl, !filled);
  if (!run) {
    int a = clkPage == 0 ? alH : tmSecs / 60, b = clkPage == 0 ? alM : tmSecs % 60;
    bigPair(8, a); rect(96, DIG_Y + 20, 8, 8, true); rect(96, DIG_Y + 74 - 28, 8, 8, true); bigPair(110, b);
    tri(50, 40, true); tri(152, 40, true); tri(50, 136, false); tri(152, 136, false);
  } else {
    uint32_t left = rtcTimerEnd - (uint32_t)time(NULL);
    if (left > 60) {                                        // minutes only, refreshed once a minute
      bigPair(8, min((uint32_t)99, (left + 59) / 60)); textLeft(EK_MID, 110, DIG_Y + 26, "min");
    } else {                                                // last minute: seconds, every second
      bigPair(8, 0); rect(96, DIG_Y + 20, 8, 8, true); rect(96, DIG_Y + 74 - 28, 8, 8, true); bigPair(110, left);
    }
  }
  if (clkPage == 0) {
    if (!timeValid()) textAt(EK_SMALL, 100, 154, L("clock not set yet", "heure pas encore r\xC3\xA9" "gl\xC3\xA9" "e"));
    else { pill(4, 152, 92, REP_NAME[alRep], false); pill(104, 152, 92, TONE_NAME[alTone], false); }
    textAt(EK_SMALL, 100, 180, L("hold: back \xC2\xB7 swipe: timer", "glisser\xC2\xA0" ": minuteur"));
  } else if (!run) {
    static const uint8_t P[6] = { 1, 3, 5, 10, 25, 60 };
    for (int i = 0; i < 6; i++) pill(4 + i * 32, 152, 29, String(P[i]), false);
    pill(40, 178, 120, String(L("ring: ", "son\xC2\xA0" ": ")) + TONE_NAME[tmTone], false);
  } else {
    time_t e = rtcTimerEnd; struct tm t; localtime_r(&e, &t); char s2[24];
    snprintf(s2, sizeof s2, L("ends %02d:%02d", "fin %02d:%02d"), t.tm_hour, t.tm_min);
    textAt(EK_MID, 100, 150, s2);
    textAt(EK_SMALL, 100, 180, L("hold: back \xC2\xB7 swipe: alarm", "glisser\xC2\xA0" ": r\xC3\xA9" "veil"));
  }
  partialOnly = run;                                        // a countdown never triggers a full refresh
  show();
  partialOnly = false;
}
static void clkSave() {
  prefs.putInt("alH", alH); prefs.putInt("alM", alM); prefs.putBool("alOn", alOn);
  prefs.putInt("alRep", alRep); prefs.putInt("alTone", alTone); prefs.putInt("tmSecs", tmSecs); prefs.putInt("tmTone", tmTone);
  rtcLastAlarmAt = 0;                       // a changed alarm may ring again today
}
static void clkEnter() { clkOn = true; clkPage = 0; clkDraw(); }
static void clkExit() { clkSave(); clkOn = false; screenIdle(); }
// step a big number: which = 0 left / 1 right, dir +1 / -1, big = accelerated
static void clkStep(int which, int dir, bool big) {
  if (clkPage == 0) {
    if (which == 0) alH = (alH + dir + 24) % 24;
    else alM = (alM + dir * (big ? 5 : 1) + 60) % 60;
    alOn = true;                            // touching the time arms the alarm
  } else {
    int m = tmSecs / 60, s = tmSecs % 60;
    if (which == 0) m = constrain(m + dir * (big ? 5 : 1), 0, 99);
    else s = (s + dir * 10 + 60) % 60;
    tmSecs = max(10, m * 60 + s);
  }
}
static void clkTouch() {
  int x0 = 0, y0 = 0, x = 0, y = 0; touchXY(x0, y0); x = x0; y = y0;
  bool run = clkPage == 1 && timerRunning();
  int mark = -1;                            // 0 up-left, 1 up-right, 2 down-left, 3 down-right
  if (!run && y0 >= 36 && y0 < 56) mark = x0 < 100 ? 0 : 1;
  if (!run && y0 >= 130 && y0 < 150) mark = x0 < 100 ? 2 : 3;
  if (mark >= 0) {                          // marks: step now, then repeat faster while held
    int which = mark & 1, dir = mark < 2 ? 1 : -1, reps = 0;
    clkStep(which, dir, false); clkDraw();
    uint32_t t0 = millis();
    while (touched() && millis() - t0 < 15000) {   // auto-repeat, capped
      if (millis() - t0 > 450) { clkStep(which, dir, ++reps > 6); clkDraw(); }
      delay(20);
    }
    lastTouch = millis(); return;
  }
  uint32_t t0 = millis(); bool moved = false, down = true, hold = false;
  while (down) {
    int nx, ny; down = touchXY(nx, ny); if (down) { x = nx; y = ny; }
    if (abs(x - x0) > SWIPE_PX || abs(y - y0) > SWIPE_PX) moved = true;
    if (!moved && down && millis() - t0 >= 600) { hold = true; break; }
    delay(12);
  }
  waitRelease();
  if (hold) { clkExit(); return; }
  if (moved) { clkPage ^= 1; clkDraw(); return; }
  if (y0 < 36 && x0 > PILL_X - 8) {         // header button: alarm on/off, timer start/stop
    if (clkPage == 0) { if (timeValid()) alOn = !alOn; }
    else if (run) { rtcTimerEnd = 0; prefs.putUInt("tmEnd", 0); }
    else { rtcTimerEnd = (uint32_t)time(NULL) + tmSecs; prefs.putUInt("tmEnd", rtcTimerEnd); }
    clkSave(); clkDraw(); return;
  }
  if (y0 >= 150 && y0 < 176) {
    if (clkPage == 0 && timeValid()) {
      if (x0 < 100) alRep = (alRep + 1) % 3;
      else { alTone = (alTone + 1) % TONE_COUNT; clkDraw(); audioEnsure(); preview(alTone); clkSave(); return; }
      clkSave(); clkDraw();
    } else if (clkPage == 1 && !run) {
      static const uint8_t P[6] = { 1, 3, 5, 10, 25, 60 };
      tmSecs = P[constrain((x0 - 4) / 32, 0, 5)] * 60; clkSave(); clkDraw();
    }
  } else if (y0 >= 176 && clkPage == 1 && !run) {          // timer ringer chip
    tmTone = (tmTone + 1) % TONE_COUNT; clkDraw(); audioEnsure(); preview(tmTone); clkSave();
  }
}

static bool audioUp = false;
static void audioEnsure() {                 // codec + recording buffer, once per wake
  if (audioUp) return;
  wk(4);                                    // v0.26.1
  ek_audio_init(); audioUp = true;
  if (!rec) rec = (uint8_t *)heap_caps_malloc(REC_MAX, MALLOC_CAP_SPIRAM);
}
static void alarmLoad() {
  alH = prefs.getInt("alH", 7); alM = prefs.getInt("alM", 30); alOn = prefs.getBool("alOn", false);
  alRep = prefs.getInt("alRep", 2); alTone = prefs.getInt("alTone", TONE_CHIME); if (alTone < 0 || alTone >= TONE_COUNT) alTone = TONE_CHIME; tmSecs = prefs.getInt("tmSecs", 300);
  tmTone = prefs.getInt("tmTone", TONE_CHIME); if (tmTone < 0 || tmTone >= TONE_COUNT) tmTone = TONE_CHIME;
  if (!rtcTimerEnd) rtcTimerEnd = prefs.getUInt("tmEnd", 0);       // survives a switch-off
  radVol = constrain(prefs.getInt("radVol", 6), 0, 10);
  if (rtcTimerEnd && rtcTimerEnd + RING_MAX_S < (uint32_t)time(NULL)) { rtcTimerEnd = 0; prefs.putUInt("tmEnd", 0); }
}
// ================================================================ CL!CKER: the 154 as a Bluetooth keyboard (v0.25.2)
// Contract v13. Keyboard mode only: the talk and the cat toy mode come later. Classic only for now.
// One paired device at a time, the chosen one (A, B, ...), or "+ new" to pair another. Wi-Fi is off while it is open.
// Inside: right third or BOOT = next, left third or PWR = previous, middle = send (Ctrl+Enter),
// bottom left = the key pair (down/up, right/left, page down/up), bottom right = dictate (voice command key),
// top = the next device, hold = back to the tiles. PWR held 2 s still switches off.
// It sleeps 5 minutes after the last press. A press of BOOT or PWR wakes it and is sent once the device is back.
// The journal gets one line per session and one per failure: never a name, an address or a key.
#if CLICKER
#include "ek_hid.h"
#define CK_AWAKE_MS   (5UL * 60 * 1000)      // awake this long after the last press
#define CK_BACK_MS    8000                   // a press waits this long for the device to come back
#define CK_SESSION_S  (30UL * 60)            // asleep longer than this since the last press: the session ends
#define CK_MAX_DEV    8
enum { CK_NEXT, CK_PREV, CK_SEND, CK_DICT };
RTC_DATA_ATTR static uint8_t rtcCkOn, rtcCkRefusedSaid;
RTC_DATA_ATTR static uint32_t rtcCkT0, rtcCkLast;           // session start, last press (seconds)
RTC_DATA_ATTR static uint16_t rtcCkTurns, rtcCkSends, rtcCkDict, rtcCkBack, rtcCkBackMax;   // counts; reconnects, longest (tenths)
static bool ckOn = false, ckBootWas = false, ckTiming = false;
static int ckPending = -1, ckShown = -1, ckSel = -1, ckDevN = 0;
static uint32_t ckPendAt = 0, ckWakeAt = 0;
static uint8_t ckDev[CK_MAX_DEV][6];

static int ckKeys() { return constrain(prefs.getInt("ckKeys", 0), 0, 2); }
static const char *ckKeysName(int k) { return k == 1 ? "\xE2\x86\x92 \xE2\x86\x90" : k == 2 ? "PgDn PgUp" : "\xE2\x86\x93 \xE2\x86\x91"; }
static const char *ckKeysLog(int k) { return k == 1 ? "right/left" : k == 2 ? "page down/up" : "down/up"; }
static String ckHex(const uint8_t *a) { char b[13]; snprintf(b, sizeof b, "%02x%02x%02x%02x%02x%02x", a[0], a[1], a[2], a[3], a[4], a[5]); return String(b); }
static void ckDevices() {                     // the paired devices, and which one is chosen
  ckDevN = ekhBonds(ckDev, CK_MAX_DEV);
  prefs.putInt("ckPaired", ckDevN);
  String t = prefs.getString("ckTarget", ""); ckSel = -1;
  for (int i = 0; i < ckDevN; i++) if (t == ckHex(ckDev[i])) ckSel = i;
  if (ckSel < 0 && ckDevN && (t.length() || !prefs.isKey("ckTarget"))) ckSel = 0;   // "" on purpose means "+ new"
}
static void ckChoose() { ekhChoose(ckSel >= 0 ? ckDev[ckSel] : NULL); ekhAdvertise(true); }
static String ckWho() { return ckSel >= 0 ? String((char)('A' + ckSel)) : String("+ new"); }

static void ckGlyph(int cx, int cy) {         // the Bluetooth rune, drawn with 3-pixel strokes
  auto ln = [](int x0, int y0, int x1, int y1) {
    int n = max(abs(x1 - x0), abs(y1 - y0));
    for (int i = 0; i <= n; i++) rect(x0 + (x1 - x0) * i / n - 1, y0 + (y1 - y0) * i / n - 1, 3, 3, true);
  };
  ln(cx, cy - 24, cx, cy + 24);
  ln(cx, cy - 24, cx + 12, cy - 12); ln(cx + 12, cy - 12, cx - 12, cy + 12);
  ln(cx, cy + 24, cx + 12, cy + 12); ln(cx + 12, cy + 12, cx - 12, cy - 12);
}
static void ckFace() {                        // the tile
  ckGlyph(100, 52);
  textAt(EK_MID, 100, 90, "Cl!cker");
  int n = prefs.getInt("ckPaired", 0);
  textAt(EK_SMALL, 100, 122, n == 0 ? String("no device paired yet") : n == 1 ? String("1 device paired") : String(n) + " devices paired");
  textAt(EK_SMALL, 100, 164, "tap to open");
}
static void ckDraw() {                        // the open Cl!cker
  epd->EPD_Clear();
  int st = ekhState();
  rdHeader("Cl!cker", "hold: back");
  String line = ckWho() + ": ";
  if (st == EKH_READY) line += "connected";
  else if (st == EKH_LINKED) line += "connecting...";
  else if (ckSel < 0) line = "pair: E!nkster Cl!cker";
  else line += "waiting for it";
  textAt(EK_SMALL, 100, 23, line);
  rect(8, 44, 184, 1, true);
  textAt(EK_BIG, 33, 78, "\xE2\x80\xB9"); textAt(EK_BIG, 167, 78, "\xE2\x80\xBA");
  textAt(EK_MID, 100, 86, "send");
  rect(66, 56, 1, 84, true); rect(133, 56, 1, 84, true);
  rect(8, 150, 184, 1, true); rect(100, 156, 1, 38, true);
  textAt(EK_SMALL, 50, 167, String("keys ") + ckKeysName(ckKeys()));
  textAt(EK_SMALL, 150, 167, "dictate");
  show();
  ckShown = st;
}
static void ckEnd(const char *why) {          // the session's one journal line; Bluetooth stops
  uint32_t secs = nowS() - rtcCkT0;
  String d = "keyboard session " + String(secs / 60) + " min " + String(secs % 60) + " s, keys " + ckKeysLog(ckKeys())
           + ", turns " + String(rtcCkTurns) + ", sends " + String(rtcCkSends) + ", dictation " + String(rtcCkDict)
           + ", transcriptions 0, cat toy 0";
  if (rtcCkBack) d += ", reconnects " + String(rtcCkBack) + " (longest " + String(rtcCkBackMax / 10.0f, 1) + " s)";
  if (why) d += ", " + String(why);
  FL("clicker", d);
  ekhStop();
  ckOn = false; rtcCkOn = 0; ckPending = -1;
}
static void ckOpen(bool fromWake) {
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF); wifiStarted = false;   // one radio job at a time
  pinMode(GPIO_NUM_0, INPUT_PULLUP);
  if (!fromWake) {
    rtcCkT0 = rtcCkLast = nowS(); rtcCkTurns = rtcCkSends = rtcCkDict = rtcCkBack = rtcCkBackMax = 0; rtcCkRefusedSaid = 0;
  }
  if (!ekhBegin("E!nkster Cl!cker", (uint8_t)lipoPct(wakeMv))) {
    FL("clicker", "FAILED: Bluetooth did not start");
    rtcCkOn = 0; screenText("Bluetooth did not start.", false); delay(2500); screenIdle(); return;
  }
  ckOn = true; rtcCkOn = 1; onIdle = false;
  ckDevices(); ckChoose();
  ckBootWas = digitalRead(GPIO_NUM_0) == LOW;
  if (!fromWake) FL("clicker", "open, " + String(ckDevN) + " devices paired");
  ckDraw();
}
static void ckClose() { ckEnd(NULL); screenIdle(); }
static void ckSendNow(int a) {
  int k = ckKeys(); bool ok = false;
  if (a == CK_NEXT) { ok = ekhKey(0, k == 1 ? EKH_RIGHT : k == 2 ? EKH_PGDN : EKH_DOWN); if (ok) rtcCkTurns++; }
  else if (a == CK_PREV) { ok = ekhKey(0, k == 1 ? EKH_LEFT : k == 2 ? EKH_PGUP : EKH_UP); if (ok) rtcCkTurns++; }
  else if (a == CK_SEND) { ok = ekhKey(EKH_CTRL, EKH_ENTER); if (ok) rtcCkSends++; }
  else if (a == CK_DICT) { ok = ekhConsumer(EKH_VOICE); if (ok) rtcCkDict++; }
}
static void ckPress(int a) {                  // a press: sent now, or kept until the device is back
  rtcCkLast = nowS(); lastTouch = millis();
  if (ekhState() == EKH_READY) { ckSendNow(a); return; }
  ckPending = a; ckPendAt = millis();         // only the latest press waits
}
static void ckWake(uint64_t pins) {           // woken with the Cl!cker open
  ckOpen(true); ckTiming = true; ckWakeAt = 0;   // timed from the wake itself: the press that woke it
  if (pins & (1ULL << GPIO_NUM_0)) ckPress(CK_NEXT);
  else if (pins & (1ULL << PWR_BTN)) {
    uint32_t t0 = millis();
    while (digitalRead(PWR_BTN) == LOW && millis() - t0 < OFF_MS) delay(20);
    if (millis() - t0 >= OFF_MS) powerOff(L("OFF", "\xC3\x89" "TEINT"));
    else ckPress(CK_PREV);
  }
}
static void ckNextDevice() {                  // A -> B -> ... -> + new -> A
  ckSel = ckSel + 1 >= ckDevN ? -1 : ckSel + 1;
  prefs.putString("ckTarget", ckSel >= 0 ? ckHex(ckDev[ckSel]) : String(""));
  ckPending = -1; ckChoose(); ckDraw();
}
static void ckLoop() {
  ekhPoll();
  uint8_t a[6];
  if (ekhTakeNew(a)) {                        // a device paired in "+ new": it becomes the chosen one
    prefs.putString("ckTarget", ckHex(a));
    ckDevices();
    FL("clicker", "paired a new device, " + String(ckDevN) + " paired now");
  }
  int st = ekhState();
  if (st == EKH_READY && ckTiming) {          // back after a wake: how long it took (contract: 1 to 3 s, to be measured)
    ckTiming = false; uint32_t t = (millis() - ckWakeAt) / 100;
    rtcCkBack++; if (t > rtcCkBackMax) rtcCkBackMax = t;
  }
  if (st != ckShown) ckDraw();
  if (ckPending >= 0) {
    if (st == EKH_READY) { int p = ckPending; ckPending = -1; ckSendNow(p); }
    else if (millis() - ckPendAt > CK_BACK_MS) {
      FL("clicker", ckSel >= 0 ? "FAILED: the device did not come back within 8 s, a key was not sent" : "FAILED: no device paired yet, a key was not sent");
      ckPending = -1; ckTiming = false;
    }
  }
  if (ekhRefused && !rtcCkRefusedSaid) { rtcCkRefusedSaid = 1; FL("clicker", "a paired device that is not the chosen one was sent away"); }
  if (ringIfDue()) ckDraw();
  if (millis() - lastTouch > CK_AWAKE_MS) goSleep();
  bool b = digitalRead(GPIO_NUM_0) == LOW;    // BOOT: next
  if (b && !ckBootWas) ckPress(CK_NEXT);
  ckBootWas = b;
  if (!touched()) { delay(20); return; }
  int x0 = 0, y0 = 0, x = 0, y = 0; touchXY(x0, y0); x = x0; y = y0;
  uint32_t t0 = millis(); bool moved = false, down = true;
  while (down) {
    int nx, ny; down = touchXY(nx, ny);
    if (down) { x = nx; y = ny; }
    if (abs(x - x0) > SWIPE_PX || abs(y - y0) > SWIPE_PX) moved = true;
    if (!moved && down && millis() - t0 >= 600) break;      // a still hold: back to the tiles
    delay(12);
  }
  if (down && !moved) { waitRelease(); ckClose(); lastTouch = millis(); return; }
  lastTouch = millis();
  if (moved) {                                // a sideways swipe turns a page too
    int dx = x - x0, dy = y - y0;
    if (abs(dx) >= abs(dy)) { bool next = dx < 0; if (SWIPE_FLIP) next = !next; ckPress(next ? CK_NEXT : CK_PREV); }
    return;
  }
  int tx = TOUCH_FLIP_X ? 199 - x0 : x0;
  if (y0 < 46) ckNextDevice();
  else if (y0 >= 150) {
    if (tx < 100) { prefs.putInt("ckKeys", (ckKeys() + 1) % 3); ckDraw(); }
    else ckPress(CK_DICT);
  }
  else ckPress(tx < 67 ? CK_PREV : tx > 133 ? CK_NEXT : CK_SEND);
}
#else
static const bool ckOn = false;
#endif

// ---------------------------------------------------------------- power off / sleep
// the wordmark screen: E!NKSTER / 154 / third line (version at start, OFF when switched off)
static void brandScreen(const char *third, bool bigThird) {
  textAt(EK_BIG, 100, 40, "E!NKSTER");
  textAt(EK_BIG, 100, 82, "154");
  if (bigThird) textAt(EK_BIG, 100, 128, third); else textAt(EK_MID, 100, 136, third);
#if EK_INVISIBLEOCEAN
  textAt(EK_SMALL, 100, 176, "Édition InvisibleOcean");
#endif
}
static void powerOff(const char *why) {
  nlSet(false, "power off");                  // v0.25.5
#if CLICKER
  if (rtcCkOn) ckEnd("switched off");         // the session's journal line, before the log closes
#endif
  batSample(); batFlush();                  // keep the log across a switch-off
  flEnd("power off");                       // the field log's clean end
  epd->EPD_Init(); epd->EPD_Clear();        // full refresh: a clean resting face
#if EK_INVISIBLEOCEAN
  if (boxMode) drawBox(); else                // still in its box: keep the welcome screen for the recipient
#endif
  brandScreen(why, true);
  epd->EPD_DisplayPartBaseImage();
  delay(300);
  gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);
  power.VBAT_POWER_OFF();                   // release the latch: battery cuts out here
  while (digitalRead(PWR_BTN) == LOW) delay(20);   // on USB we keep running
  delay(200);
  epd->EPD_Init_Partial(); needFull = true;
  screenIdle();
}

// The cell is empty: say so on the paper (it stays there unpowered), put the logs on flash, open the latch.
// Returns only when something else still powers the board (USB): then the device carries on.
static void batteryEmptyOff() {
#if CLICKER
  if (rtcCkOn) ckEnd("battery empty");
#endif
  batFlush();
  flEnd("battery empty");
  epd->EPD_Init(); epd->EPD_Clear();
  rect(48, 22, 96, 4, true); rect(48, 66, 96, 4, true); rect(48, 22, 4, 48, true); rect(140, 22, 4, 48, true);   // a large empty cell
  rect(144, 36, 8, 20, true);
  rect(56, 30, 6, 32, true);                                                                                      // the last sliver
  textAt(EK_MID, 100, 82, L("battery empty", "batterie vide"));
  textAt(EK_SMALL, 100, 114, L("plug in USB,", "branchez l\xE2\x80\x99" "USB,"));
  textAt(EK_SMALL, 100, 132, L("then press PWR", "puis appuyez sur PWR"));
  String foot = mvText(wakeMv);
  if (timeValid()) { time_t e = time(NULL); struct tm t; localtime_r(&e, &t); char b[24];
    snprintf(b, sizeof b, L("%02d.%02d. %02d:%02d", "%02d/%02d %02d:%02d"), t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min); foot = String(b) + "  \xC2\xB7  " + foot; }
  textAt(EK_SMALL, 100, 172, foot);
  epd->EPD_DisplayPartBaseImage();
  delay(300);
  gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);
  power.VBAT_POWER_OFF();                   // on battery, it ends here
  delay(1500);
  power.VBAT_POWER_ON();                    // still alive: USB is in. Start counting again and carry on.
  rtcLowN = rtcEmptyN = 0;
  epd->EPD_Init_Partial(); needFull = true;
}

static void goSleep() {
  if (rtcNlOn) {                               // v0.25.5: the night light's 15 minutes
    if (userSession) rtcNlUntil = nowS() + NL_IDLE_S;            // a person used it: 15 minutes from now
    else if (nowS() + 2 >= rtcNlUntil) { nlSet(false, "15 min without a touch"); if (tile == T_DEVICE && onIdle) screenIdle(); }
  }
  commitNtp();
  flSleep(userSession);
#if EK_FIELDLOG
  if (flLen && (userSession || flWakeNoted || !rtcFlAt || nowS() - rtcFlAt >= 2 * 3600)) { flFlush(); rtcFlAt = nowS(); }   // v0.26.1
#endif
  rdSaveRtc();
  sdUnmount();
  rtcMagic = RTC_MAGIC;
  rtcTile = tile; rtcIdle = onIdle; rtcJarAnswered = jarAnswered;
  rtcOrAt = orAt; rtcJarAt = jarAt;
  keep(rtcOr, sizeof rtcOr, orBody, NULL);
  keep(rtcJar, sizeof rtcJar, jarBody, NULL);
  keep(rtcLast, sizeof rtcLast, lastAnswer, NULL); rtcLastGrammar = lastGrammar;

  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  if (userSession && rtcGhost >= GHOST_CLEAN) {    // walking away: clean the same image once, quietly
    epd->EPD_Init(); epd->EPD_DisplayPartBaseImage(); epd->EPD_Init_Partial(); rtcGhost = 0;
  }
  bool keepPaper = ((tile == T_CLOCK || tile == T_KAFFEE || tile == T_CLIMATE) && onIdle) || rdOn || ckOn;   // clock, reader: panel naps with its memory
  if (keepPaper) {
    epd->EPD_SendCommand(0x10); epd->EPD_SendData(0x01);   // panel deep sleep, memory kept
    delay(5);
    gpio_hold_en((gpio_num_t)EPD_RST_PIN); gpio_hold_en((gpio_num_t)EPD_CS_PIN);
  } else power.POWEER_EPD_OFF();                // the paper keeps its picture unpowered
  rtcPaperKept = keepPaper;
  power.POWEER_Audio_OFF();
  gpio_hold_en((gpio_num_t)EPD_PWR_PIN); gpio_hold_en((gpio_num_t)Audio_PWR_PIN);
  rtc_gpio_hold_en((gpio_num_t)VBAT_PWR_PIN);          // keep the battery latch closed
  bmpHold = false; bmpPower(rtcNlOn); gpio_hold_en((gpio_num_t)BMP_PWR_PIN);   // v0.25.4: the pressure sensor stays off through sleep (v0.25.5: unless the night light is on; v0.26.2: off = the pin released, held released)
  gpio_deep_sleep_hold_en();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // needed for the pull-ups
  uint64_t mask = (1ULL << PWR_BTN) | (1ULL << GPIO_NUM_0);
  rtc_gpio_pullup_en(PWR_BTN); rtc_gpio_pulldown_dis(PWR_BTN);
  if (!rtcTouchWakeOff) {
    mask |= 1ULL << TOUCH_INT;
    rtc_gpio_pullup_en(TOUCH_INT); rtc_gpio_pulldown_dis(TOUCH_INT);
  }
  esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  uint64_t us = 0;
  // seconds until a face's data is due. Already overdue means the refresh did not come through at this wake (no
  // network, no answer): try again after "retry", not after 60 s. v0.23.2: before, an overdue face woke every minute.
  auto left = [](uint32_t at, uint32_t every, uint32_t retry) -> uint64_t {
    uint32_t age = nowS() - at; return (uint64_t)(age >= every - 60 ? retry : every - age);
  };
  bool bg = !rtcReserve;                       // in reserve: no background fetching, so no wake for it either
  if (bg && onIdle && tile == T_CLIMATE && wifiConfigured()) us = left(wx.at, WX_STALE_S, WX_STALE_S) * 1000000ULL;
  if (bg && onIdle && tile == T_ORACLE)  us = left(orAt, ORACLE_STALE_S, NET_BACKOFF_S) * 1000000ULL;
  if (onIdle && tile == T_CLOCK) {             // wake just after the next minute begins
    struct timeval tv; gettimeofday(&tv, NULL);
    us = (uint64_t)(60 - tv.tv_sec % 60) * 1000000ULL - tv.tv_usec + 300000ULL;
  }
  { uint64_t pu = potdTimerUs(); if (pu && (!us || pu < us)) us = pu; }   // daily prompt at 07:00
#if EK_LOG
  {                                           // also wake for the next battery reading (and its log sample)
    // v0.23.2: counted in seconds since power-on. Before, it was counted from the last log sample, and a unit that
    // could not sample (clock never set, or no cell) woke every 60 s for nothing.
    uint32_t every = batEvery(), since = rtcWatchAt ? nowS() - rtcWatchAt : every;
    uint64_t logUs = (uint64_t)(since >= every ? 60 : every - since) * 1000000ULL;
    if (!us || logUs < us) us = logUs;
  }
#endif
  { uint64_t eu = eventWakeUs(); if (eu && (!us || eu < us)) us = eu; }   // alarm, snooze, timer
  { uint64_t ku = ksWakeUs(); if (ku && (!us || ku < us)) us = ku; }     // Kaffeesatz morning check
  if (rtcNlOn) {                                                         // v0.25.5: wake to put the night light out
    uint32_t now = nowS(); uint64_t nu = (uint64_t)(rtcNlUntil > now ? rtcNlUntil - now : 1) * 1000000ULL;
    if (!us || nu < us) us = nu;
  }
  if (us) esp_sleep_enable_timer_wakeup(us);
  rtcSleptAt = nowS();
  wk(1);                                       // v0.26.1: asleep
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------- power button
static bool pwrArmed = false;               // true once PWR was released after boot
static void checkPower() {
  bool down = digitalRead(PWR_BTN) == LOW;
  if (!down) { pwrArmed = true; return; }
  if (!pwrArmed) return;                    // still the press that switched us on
  uint32_t t0 = millis();
  while (digitalRead(PWR_BTN) == LOW && millis() - t0 < OFF_MS) delay(20);
  lastTouch = millis();
#if CLICKER
  if (millis() - t0 < OFF_MS && ckOn) { ckPress(CK_PREV); return; }   // in the Cl!cker, PWR is "previous"
#endif
  if (millis() - t0 < OFF_MS) { if (rdOn) rdExit(); if (clkOn) clkExit(); if (ksOn) ksExit();
#if CLIMATE
    if (cxOn) cxExit();
#endif
#if BATLOG
    if (bxOn) bxExit();
#endif
    return; }   // short press: leave the deep dive
  powerOff(L("OFF", "\xC3\x89" "TEINT"));
  pwrArmed = false; delay(200); pwrArmed = true;
}

// ---------------------------------------------------------------- talking
static void beep(int hz, int ms) {          // short soft tone: "speak now"
  int frames = EK_SAMPLE_RATE * ms / 1000;
  int16_t *b = (int16_t *)malloc(frames * 4); if (!b) return;
  for (int i = 0; i < frames; i++) {
    float env = i < 160 ? i / 160.0f : (frames - i < 160 ? (frames - i) / 160.0f : 1.0f);
    int16_t v = (int16_t)(sinf(2.0f * PI * hz * i / EK_SAMPLE_RATE) * 6000 * env);
    b[i * 2] = v; b[i * 2 + 1] = v;
  }
  esp_codec_dev_write(get_playback_handle(), b, frames * 4);
  free(b);
}
// brief notice, then back to what was on the paper before the hold
static void nothingHeard(bool toIdle, bool fresh) {
  screenText("didn't catch that.", false);
  delay(1200);
  if (toIdle || !lastAnswer.length()) { screenIdle(); return; }
  if (lastGrammar) screenGrammar(lastAnswer); else screenText(lastAnswer, lastAnswer.length() < 24);
}

static void talk(bool fromSleep) {
  bool wasIdle = onIdle;
  if (!fromSleep) screenText("listening...", false);
  beep(1320, 70);                           // speak after the beep
  uint32_t got = 0, start = millis();
  const uint32_t CHUNK = 2048;
  while (got + CHUNK <= REC_MAX) {
    if (ek_audio_read(rec + got, CHUNK) != 0) break;
    got += CHUNK;
    if (((got / CHUNK) % 4) == 0 && !touched()) break;     // check touch every ~128 ms
  }
  if (millis() - start < MIN_MS) {
    if (wasIdle) screenIdle("hold a bit longer"); else nothingHeard(false, false);
    return;
  }

  // stereo -> mono (left channel): compact in place (safe: read index >= write
  // index), then slide right by 44 bytes with memmove (overlap-safe) for the header
  uint32_t frames = got / 4;
  int16_t *s = (int16_t *)rec;
  int peak = 0;
  for (uint32_t i = 0; i < frames; i++) {
    s[i] = s[i * 2];
    if (i > 1600) { int a = abs(s[i]); if (a > peak) peak = a; }   // skip the first 0.1 s (beep, codec wake)
  }
  if (peak < SILENT_PEAK) { nothingHeard(wasIdle, false); return; }
  uint32_t dataLen = frames * 2;
  memmove(rec + 44, rec, dataLen);
  writeWavHeader(rec, dataLen);

  screenText("thinking...", false);
  if (rtcAskN < 255) rtcAskN++;
  String body = callBrainF(tileMode(tile), "audio/wav", rec, 44 + dataLen, true);

  // the brain answers an empty question with the tile's own face: nothing was heard
  if (jsonField(body, "error") == "heard nothing") { nothingHeard(wasIdle, false); return; }
  bool face = body.indexOf("\"payload\":") >= 0 &&
              (tile != T_JAR || body.indexOf("\"phase\":\"q\"") >= 0);
  if (face && (tile == T_ORACLE || tile == T_JAR)) {
    if (tile == T_ORACLE) { orBody = body; orAt = nowS(); }
    else { jarBody = body; jarAt = nowS(); jarAnswered = false; }   // the brain moved on to this question
    nothingHeard(true, true);
    return;
  }

  String answer = jsonField(body, "answer");
  if (!answer.length()) {
    String e = jsonField(body, "error");
    answer = e.startsWith("no Wi-Fi") ? e
           : e.length() ? "Brain says: " + e : "Odd reply.";
  }
  lastAnswer = answer; lastGrammar = tile == T_DERDIEDAS;
  if (lastGrammar) screenGrammar(answer);
  else screenText(answer, answer.length() < 24);
  if (tile == T_JAR) jarAnswered = true;       // next visit brings a fresh question
}

static void stepTile(int dir) {             // +1 next, -1 previous
  int pos = tilePos(tile); if (pos < 0) pos = 0;
  tile = TILES[(pos + dir + N_TILES) % N_TILES];
  FL("tile", flTileName(tile));
  prefs.putInt("tile", tile);
  screenIdle();
}
static void swipe(int d) {                  // d = signed travel along the dominant axis
  bool next = d < 0;                        // finger moved left (or up): next tile
  if (SWIPE_FLIP) next = !next;
  stepTile(next ? 1 : -1);
}

// a tap on a tile: middle third goes deeper (if there is a deeper level), sides do nothing;
// a tap on an answer returns to the tile. Swipes change tiles.
static int tapY = 0;                           // v0.25.5: where the last tap landed, top to bottom
static void tap(int x) {
  if (!onIdle) { screenIdle(); return; }
  if (tile == T_DEVICE && tapY >= NL_Y) { nlSet(!rtcNlOn, "by hand"); screenIdle(); return; }   // v0.25.5: the lamp switch, the whole width
  int tx = TOUCH_FLIP_X ? 199 - x : x;
  if (tx < 67 || tx > 133) return;
  if ((tile == T_ORACLE || tile == T_JAR || tile == T_KAFFEE) && !wifiConfigured()) return;   // the face says why
  if (tile == T_EDITIONS) rdEnter();
  else if (tile == T_CATALOG) catEnter();
#if CLICKER
  else if (tile == T_CLICKER) ckOpen(false);
#endif
  else if (tile == T_CLOCK) clkEnter();
  else if (tile == T_ORACLE || tile == T_JAR) fetchFace();
  else if (tile == T_CLIMATE) climEnter();
  else if (tile == T_DEVICE) snEnter();      // v0.25.3: the Sensors page
  else if (tile == T_RADIO) {
    if (!wifiConfigured()) return;             // v0.24.0: the radio's face says it (before, the message was drawn over the face)
    radioRun();
  }
#if BATLOG
  else if (tile == T_BATTERY) bxEnter();
#endif
  else if (tile == T_KAFFEE) {                  // read it; without today's edito, check now first
    if (!ksFresh() && ksSetUp()) { textAt(EK_SMALL, 100, 178, "checking..."); show(); if (ksFetch() || !ksBrief.length()) { ksFace(); if (!ksBrief.length()) return; } }
    if (ksBrief.length()) ksOpen();
  }
}

// ---------------------------------------------------------------- escape hatch
// Its own task, so it works even if the main loop is stuck: PWR held 5 s = off (on battery)
// or restart (on USB). The normal 2 s hold still gives the tidy "OFF" screen first.
static void pwrGuard(void *) {
  while (digitalRead(PWR_BTN) == LOW) vTaskDelay(pdMS_TO_TICKS(50));   // not the press that woke us
  uint32_t downSince = 0;
  for (;;) {
    if (digitalRead(PWR_BTN) == LOW) {
      if (!downSince) downSince = millis();
      else if (millis() - downSince > 5000) {
        gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);
        power.VBAT_POWER_OFF();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
      }
    } else downSince = 0;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------------------------------------------------------------- field log: start, marks, the export screen
static const char *flTileName(int t) {
  switch (t) {
    case T_ASK: return "ask"; case T_DERDIEDAS: return "der die das"; case T_ORACLE: return "oracle"; case T_JAR: return "jar";
    case T_KAFFEE: return "kaffeesatz"; case T_RADIO: return "radio"; case T_WEATHER: return "weather"; case T_CLOCK: return "clock";
    case T_CLIMATE: return "climate"; case T_EDITIONS: return "editions"; case T_CATALOG: return "catalog"; case T_BATTERY: return "battery"; case T_DEVICE: return "device"; case T_CLICKER: return "clicker";
    default: return "?";
  }
}
#if EK_FIELDLOG
#include <WebServer.h>
#include <DNSServer.h>
static const char *flResetText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_DEEPSLEEP: return "power on (PWR pressed while it charged, off)";
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_USB:       return "reset over USB (flashing tool)";
    case ESP_RST_SW:        return "restart asked by the firmware";
    case ESP_RST_PANIC:     return "restart after a crash";
    case ESP_RST_CPU_LOCKUP: return "restart after a crash (processor locked up)";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "restart by the watchdog";
    case ESP_RST_BROWNOUT:  return "restart after the supply dipped (brownout)";
    default:                return "restart, cause unknown";
  }
}
static uint32_t flMarkAt = 0; static bool flBootWas = false;
static void flMark(bool quiet) {
  flMarks++; flMarkAt = millis();
  flEvent("MARK", "MARK " + String(flMarks));
  if (!quiet && audioUp) beep(1760, 60);       // a short high note: the mark is in
}
// Called once per start, after the clock is back. A wake from deep sleep carries the run on; any other start
// first settles the previous run (did it end cleanly?) and then opens a new one.
static void flBoot(esp_sleep_wakeup_cause_t cause, uint64_t pins, bool woke) {
  pinMode(GPIO_NUM_0, INPUT_PULLUP);           // BOOT button = Mark
  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_DEEPSLEEP && woke && flRingOk() && flRun && esp_rtc_get_time_us() >= flT0) {
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {      // a person woke it
      flWakeNoted = true;
#if CLICKER
      if ((pins & (1ULL << GPIO_NUM_0)) && rtcCkOn) flLine("wake", "BOOT button (Cl!cker: next)");   // v0.25.2: no MARK in the Cl!cker
      else
#endif
      if (pins & (1ULL << GPIO_NUM_0)) { flLine("wake", "BOOT button"); flMark(true); }
      else flLine("wake", (pins & (1ULL << TOUCH_INT)) ? "touch" : "PWR button");
    } else flWakePending = "timer";            // a quiet wake: said only if something happens in it
    flBootWas = digitalRead(GPIO_NUM_0) == LOW;
    return;
  }
  bool ringOk = flRingOk();
  const char *badWas = NULL;                   // v0.25.6
  if (prefs.getBool("flOpen", false)) {        // the last run never reached its end line
    const char *bad = NULL;
    switch (rr) {
      case ESP_RST_PANIC: case ESP_RST_CPU_LOCKUP: bad = "crash"; break;
      case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: bad = "watchdog"; break;
      case ESP_RST_BROWNOUT: bad = "brownout"; break;
      case ESP_RST_SW: bad = "forced restart"; break;                      // PWR held 5 s on USB
      case ESP_RST_POWERON: case ESP_RST_DEEPSLEEP: bad = "power cut"; break;   // battery empty, unplugged, or PWR held 5 s
      default: break;                                                        // reset from outside (USB, flashing): not a fault
    }
    if (ringOk && flRun) flLine(bad ? "crash" : "end", bad ? String("no clean end: ") + bad : String("reset from outside (USB or flashing tool)"));
    else {                                     // the memory went with the power: say so in plain words, under the old run
      if (!ringOk) { flMagic = FL_MAGIC; flLen = 0; flLost = 0; }
      char b[120]; unsigned long was = prefs.getUInt("flRun", 0);
      if (bad) snprintf(b, sizeof b, "!! run %lu had no clean end (%s): its last lines went with the power", was, bad);
      else snprintf(b, sizeof b, "(run %lu was reset from outside)", was);
      flRaw(b);
    }
    if (bad) prefs.putString("flBad", bad);
    badWas = bad;
  }
  if (!flRingOk()) { flMagic = FL_MAGIC; flLen = 0; flLost = 0; }
  uint8_t wkWas = wkBoot;                       // v0.26.1: as the last run left it
  flStartRun(flResetText(rr));
  if (badWas) {                                // v0.26.1: where the last run was when it ended
    static const char *WK[] = { "", "asleep", "awake", "awake, with Wi-Fi started", "awake, with sound started" };
    if (wkWas >= 1 && wkWas <= 4) flEvent("power", String("the last run ended ") + WK[wkWas]);
  }
  if (esp_core_dump_image_check() == ESP_OK) {  // v0.26.1: a crash report waits on flash (the newest crash only)
    esp_core_dump_summary_t *cs = (esp_core_dump_summary_t *)calloc(1, sizeof(esp_core_dump_summary_t));
    char why[96] = "";
    if (cs && esp_core_dump_get_summary(cs) == ESP_OK) {
      esp_core_dump_get_panic_reason(why, sizeof why);
      for (char *c = why; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
      char b[200]; int k = snprintf(b, sizeof b, "saved report: task %.15s, at 0x%08lx, cause %lu%s%s", cs->exc_task, (unsigned long)cs->exc_pc,
                                    (unsigned long)cs->ex_info.exc_cause, why[0] ? ", " : "", why);
      flEvent("crash", String(b));
      String bt = "backtrace";
      for (uint32_t i = 0; i < cs->exc_bt_info.depth && i < 16; i++) { char a[12]; snprintf(a, sizeof a, " 0x%08lx", (unsigned long)cs->exc_bt_info.bt[i]); bt += a; }
      if (cs->exc_bt_info.corrupted) bt += " (cut short)";
      bt += ", build " + String((const char *)cs->app_elf_sha256).substring(0, 8);
      flEvent("crash", bt);
      (void)k;
    } else flEvent("crash", "a saved report is on flash but cannot be read");
    free(cs);
    esp_core_dump_image_erase();               // said once
  }
  {                                            // v0.25.6: was the sensor being switched when the last run ended?
    static const char *ST[] = { "", "while the sensor was being switched on", "with the sensor on", "while the sensor was being switched off" };
    uint32_t st = prefs.getUInt("bmpStep", 0);
    if (st >= 1 && st <= 3) flEvent("sensor", String("the last run ended ") + ST[st] + " (GP2)");
    else if (badWas) flEvent("sensor", "the last run did not end while the sensor was switched or on");
    if (st) bmpStep(0);
    flEvent("sensor", "GP2 is released when the sensor is off (v0.26.2), not driven low");
  }
  flFlush();                                   // the header is on flash before anything else happens
  flBootWas = digitalRead(GPIO_NUM_0) == LOW;
}
static void flPoll(bool quiet) {               // BOOT pressed while awake: a mark
  bool down = digitalRead(GPIO_NUM_0) == LOW;
  if (down && !flBootWas && millis() - flMarkAt > 300) { flMark(quiet); lastTouch = millis(); }
  flBootWas = down;
}
static void flSleep(bool person) {             // quiet timer wakes with nothing to say leave no line
  if (!person && !flWakeNoted) return;
  flEvent("sleep", "after " + flSecs(millis()) + " awake, tile " + flTileName(tile));
}
static String flBad() { return prefs.isKey("flBad") ? prefs.getString("flBad", "") : String(""); }
static String flNotice() {                     // for the start screen, only when the last run ended badly
  String b = flBad();
  return b.length() ? "\n\n! last run: " + b + "\nlogs: hold Battery tile" : String("");
}

// ---- the logs page: the device serves all its logs on one page; the phone reads them and copies what it needs.
// On the same Wi-Fi: scan the code. Without Wi-Fi (or on a network that keeps devices apart): the device opens
// its own hotspot, and the page is at 192.168.4.1. The page is plain http and holds the logs, nothing else.
//   /            the page: events (the field log) and measurements (the 30-minute log), each with its Copy button
//   /log.txt     events, plain text            /data.csv    measurements, a table
//   /all.txt     both in one file              /log.bad     a 30-minute log that could not be read, as it was found
static WebServer *flSrv = NULL; static uint32_t flServed = 0;
static void flSendFile(const char *path, bool esc) {
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r"); if (!f) return;
  static char b[513]; int k;
  while ((k = f.read((uint8_t *)b, 512)) > 0) {
    if (esc) { b[k] = 0; String s = b; s.replace("&", "&amp;"); s.replace("<", "&lt;"); if (s.length()) flSrv->sendContent(s); }
    else flSrv->sendContent(b, k);
  }
  f.close();
}
static void flSendLog(bool esc) {              // events: everything kept, oldest first
  flFlush();
  uint32_t drop = prefs.getUInt("flDrop", 0);
  if (drop) flSrv->sendContent(String(drop) + " earlier lines dropped\n");
  if (lfsMount()) { flSendFile("/field.old", esc); flSendFile("/field.txt", esc); }
  if (flRingOk() && flLen) flSrv->sendContent(flBuf, flLen);   // only when flash refused them
  flServed++;
  if (flBad().length()) prefs.putString("flBad", "");           // the bad end has been seen
}
static int flSendData() {                      // measurements: every record kept, oldest first; returns how many
  int n; BatSample *a = loadLog(n);
  flSrv->sendContent(LOG_CSV_HEAD);
  static char chunk[1400]; int used = 0;
  for (int i = 0; i < n; i++) {
    used += logCsvLine(a[i], chunk + used, sizeof chunk - used);
    if (used > (int)sizeof chunk - 120) { flSrv->sendContent(chunk, used); used = 0; }
  }
  if (used) flSrv->sendContent(chunk, used);
  if (a) free(a);
  flServed++;
  return n;
}
static String flDevName() { uint64_t mac = ESP.getEfuseMac(); char dev[24]; snprintf(dev, sizeof dev, "E!nkster-%02X%02X", (int)((mac >> 32) & 0xFF), (int)((mac >> 40) & 0xFF)); return String(dev); }
#define FL_FLAGS_NOTE "flags: 1 = read at a cold start (the battery reads about 50 mV low), 2 = taken in reserve (a reading every 5 min), 3 = both"
static void flPage() {
  flSrv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  flSrv->send(200, "text/html; charset=utf-8", String(
    "<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>E!nkster logs</title>"
    "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;font-size:19px;line-height:1.5;margin:0;padding:22px;"
    "background:#f3efe6;color:#1c1b19}h1{font-size:26px;margin:0 0 8px}h2{font-size:24px;margin:34px 0 4px}"
    "button,a.b{display:block;box-sizing:border-box;width:100%;margin:14px 0;font-size:19px;font-weight:700;padding:14px;border:0;"
    "border-radius:10px;background:#1c1b19;color:#f3efe6;text-align:center;text-decoration:none}"
    "a.b{background:#f3efe6;color:#1c1b19;border:2px solid #1c1b19}"
    "textarea{display:block;width:100%;height:44vh;box-sizing:border-box;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:14px;"
    "line-height:1.45;padding:10px;border:2px solid #1c1b19;border-radius:8px;background:#fff;color:#1c1b19;white-space:pre;overflow:auto}"
    ".n{color:#6b665c;font-size:16px;margin:0 0 8px}</style>"
    "<h1>E!nkster logs</h1><p class=n>") + flDevName() + " \xC2\xB7 " EK_VERSION " \xC2\xB7 Classic</p>"
    "<p class=n>Nothing leaves the device unless you copy it. Take the part you need.</p>"
    "<h2>Events</h2><p class=n>The field log: what the device did, line by line.</p>"
    "<button type=button data-t=t1>Copy the events</button><p class=n id=m1></p>"
    "<textarea id=t1 readonly wrap=off spellcheck=false>");
  flSendLog(true);
  flSrv->sendContent(
    "</textarea><a class=b href=/log.txt>Open the events as text</a>"
    "<h2>Measurements</h2><p class=n>Every 30 minutes: battery, room, outdoors, pressure. " FL_FLAGS_NOTE ".</p>"
    "<button type=button data-t=t2>Copy the measurements</button><p class=n id=m2></p>"
    "<textarea id=t2 readonly wrap=off spellcheck=false>");
  flSendData();
  String tail = "</textarea><a class=b href=/data.csv>Open the measurements as a table (CSV)</a>"
    "<h2>Both</h2><a class=b href=/all.txt>Open both in one file</a>";
  if (LittleFS.exists("/log.bad")) tail += "<h2>Set aside</h2><p class=n>An older 30-minute log this version could not read. It is kept as it was found.</p><a class=b href=/log.bad>Download it</a>";
  flSrv->sendContent(tail +
    "<script>var a=document.querySelectorAll('textarea');for(var i=0;i<a.length;i++)a[i].scrollTop=a[i].scrollHeight;"
    "var b=document.querySelectorAll('button');for(var j=0;j<b.length;j++)b[j].onclick=function(){"
    "var t=document.getElementById(this.getAttribute('data-t')),m=this.nextElementSibling,ok=false;t.focus();t.select();"
    "try{t.setSelectionRange(0,t.value.length)}catch(e){}try{ok=document.execCommand('copy')}catch(e){}"
    "m.textContent=ok?'Copied. Paste it into the chat.':'Copy did not work here. Press and hold in the box, choose Select all, then Copy.'};</script>");
  flSrv->sendContent("");
}
static void flText() {
  flSrv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  flSrv->send(200, "text/plain; charset=utf-8", "");
  flSendLog(false);
  flSrv->sendContent("");
}
static void flCsv() {
  flSrv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  flSrv->send(200, "text/csv; charset=utf-8", "");
  flSendData();
  flSrv->sendContent("");
}
static void flAll() {                          // one file: the events, then the measurements
  flSrv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  flSrv->send(200, "text/plain; charset=utf-8", "");
  flSendLog(false);
  flSrv->sendContent(String("\nMEASUREMENTS\napp       einkster154\nversion   " EK_VERSION "\ndevice    ") + flDevName() + "\nnote      " FL_FLAGS_NOTE "\n");
  flSendData();
  flSrv->sendContent("");
}
static void flBadFile() {
  if (!LittleFS.exists("/log.bad")) { flSrv->send(404, "text/plain; charset=utf-8", "nothing was set aside"); return; }
  File f = LittleFS.open("/log.bad", "r"); flSrv->streamFile(f, "application/octet-stream"); f.close();
}
static void flQr(const String &text, int cy, int box) {
  esp_qrcode_config_t cfg = { qrGrab, 10, ESP_QRCODE_ECC_MED }; qrSize = 0;
  if (esp_qrcode_generate(&cfg, text.c_str()) != ESP_OK || !qrSize) return;
  int sc = max(1, min(5, box / (qrSize + 2))), side = qrSize * sc, x0 = 100 - side / 2, y0 = cy - side / 2;
  for (int r = 0; r < qrSize; r++) for (int c = 0; c < qrSize; c++) if (qrM[r * qrSize + c]) rect(x0 + c * sc, y0 + r * sc, sc, sc, true);
}
static bool flServe(bool ap) {                 // true = the user asked for the hotspot instead
  uint64_t mac = ESP.getEfuseMac(); char name[24]; snprintf(name, sizeof name, "E!nkster-%02X%02X", (int)((mac >> 32) & 0xFF), (int)((mac >> 40) & 0xFF));
  DNSServer dns; String where;
  if (ap) {
    WiFi.disconnect(true); WiFi.mode(WIFI_AP); delay(100);
    WiFi.softAP(name); delay(200);
    dns.start(53, "*", WiFi.softAPIP()); where = "192.168.4.1";
  } else { WiFi.setSleep(false); where = WiFi.localIP().toString(); }
  WebServer srv(80); flSrv = &srv;
  srv.on("/", flPage); srv.on("/log.txt", flText); srv.on("/data.csv", flCsv); srv.on("/all.txt", flAll); srv.on("/log.bad", flBadFile);
  srv.onNotFound(flPage);                       // any address: the page (so the hotspot opens it by itself)
  srv.begin();
  String bad = flBad();
  epd->EPD_Clear();
  textAt(EK_SMALL, 100, 1, "logs  \xC2\xB7  run " + String((unsigned long)flRun));
  flQr(ap ? String("WIFI:S:") + name + ";T:nopass;;" : "http://" + where + "/", 68, 96);
  textAt(EK_SMALL, 100, 120, ap ? String("1. Wi-Fi ") + name : where);
  textAt(EK_SMALL, 100, 138, ap ? "2. open 192.168.4.1" : "scan on the same Wi-Fi");
  textAt(EK_SMALL, 100, 156, "last run: " + (bad.length() ? bad : String("clean")));
  textAt(EK_SMALL, 100, 180, ap ? "hold: back" : "tap hotspot \xC2\xB7 hold back");
  show();
  uint32_t t0 = millis(), down0 = 0, seen = flServed; bool again = false;
  while (millis() - t0 < 5UL * 60 * 1000) {    // five quiet minutes close it
    if (ap) dns.processNextRequest();
    srv.handleClient();
    if (flServed != seen) { seen = flServed; t0 = millis(); }
    if (touched()) {
      if (!down0) down0 = millis();
      else if (millis() - down0 > 800) break;                    // hold: back
    } else if (down0) {
      if (!ap && millis() - down0 > 40) { again = true; break; }  // tap: the hotspot instead
      down0 = 0;
    }
    if (digitalRead(PWR_BTN) == LOW) break;
    delay(2);
  }
  srv.stop(); flSrv = NULL;
  if (ap) { dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); wifiStarted = false; }
  else WiFi.setSleep(true);
  waitRelease();
  return again;
}
static void flScreen() {
  waitRelease();
  flEvent("log", "logs page opened");
  screenText("logs\nconnecting...", false);
  flServed = 0;
  bool ap = !wifiUp();
  while (flServe(ap)) ap = true;
  flEvent("log", flServed ? "logs page closed, something was read" : "logs page closed, nothing was read");
  lastTouch = millis(); screenIdle();
}
#else
static void flBoot(esp_sleep_wakeup_cause_t, uint64_t, bool) {}
static void flPoll(bool) {}
static void flSleep(bool) {}
static void flScreen() {}
static String flNotice() { return String(""); }
#endif

// ---------------------------------------------------------------- start / wake
// Started by the charger, not by the button: stay off. USB powers the board whatever we do, so the
// processor starts; we go straight back to sleep without latching the battery or touching the screen.
// The charging chip charges on its own. PWR wakes it normally; unplugging cuts it off completely.
static void chargeOffIfUsbStart() {
  if (esp_reset_reason() != ESP_RST_POWERON) return;                 // a reset or a wake: carry on
  pinMode(PWR_BTN, INPUT_PULLUP); delayMicroseconds(200);
  if (digitalRead(PWR_BTN) == LOW) return;                           // switched on with the button
  power.VBAT_POWER_OFF();                                             // battery stays out of it,
  gpio_hold_en((gpio_num_t)VBAT_PWR_PIN); gpio_deep_sleep_hold_en();  // held open through sleep: unplugged = truly off
  rtcMagic = 0;                                                       // the next wake is a fresh start
  rtc_gpio_init(PWR_BTN); rtc_gpio_set_direction(PWR_BTN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(PWR_BTN); rtc_gpio_pulldown_dis(PWR_BTN);
  esp_sleep_enable_ext1_wakeup(1ULL << PWR_BTN, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

void setup() {
  chargeOffIfUsbStart();                    // plugged in while off: charge, stay off
  power.VBAT_POWER_ON();                    // latch battery power FIRST, or the board dies off USB
  wakeMv = batteryMv();
  gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);  // (it was held through sleep)
  gpio_hold_dis((gpio_num_t)EPD_PWR_PIN); gpio_hold_dis((gpio_num_t)Audio_PWR_PIN);
  gpio_hold_dis((gpio_num_t)EPD_RST_PIN); gpio_hold_dis((gpio_num_t)EPD_CS_PIN);
  if (rtcNlOn) { pinMode(BMP_PWR_PIN, OUTPUT); gpio_set_drive_capability((gpio_num_t)BMP_PWR_PIN, GPIO_DRIVE_CAP_1); digitalWrite(BMP_PWR_PIN, HIGH); bmpOn = true; }   // v0.25.5: set before the hold lets go, so the light does not blink
  else { pinMode(BMP_PWR_PIN, INPUT); gpio_set_pull_mode((gpio_num_t)BMP_PWR_PIN, GPIO_FLOATING); }   // v0.26.2: released, as it was held
  gpio_hold_dis((gpio_num_t)BMP_PWR_PIN);   // v0.25.4: the pressure sensor's power (off until a reading wants it; v0.26.2: released, not low)
  gpio_deep_sleep_hold_dis();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  uint64_t pins = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
  bool woke = rtcMagic == RTC_MAGIC && (cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_TIMER);
  prefs.begin("einkster", false);
  wkBoot = prefs.getUChar("wkStep", 0);     // v0.26.1: before anything writes it
  if (prefs.isKey(EK_SET_LANG)) { String lg = prefs.getString(EK_SET_LANG, ""); if (lg == "fr") uiFr = true; else if (lg == "en") uiFr = false; }   // v0.26.0
  if (prefs.isKey(EK_SET_ALT)) altM = constrain(prefs.getInt(EK_SET_ALT, EK_ALT_M), -50, 1500);                        // v0.26.0
#if EK_INVISIBLEOCEAN
  boxMode = !prefs.getBool("unboxed", false);
#endif
  bool reading = woke ? rtcRdOn : prefs.getBool("rdOn", false);
#if CLICKER
  bool clicking = woke && rtcCkOn;           // v0.25.2: the Cl!cker was open: Bluetooth, never Wi-Fi
#else
  bool clicking = false;
#endif
  if (cause != ESP_SLEEP_WAKEUP_TIMER && !reading && !clicking) wifiStart();   // connect while the user talks (never when reading)

  pinMode(PWR_BTN, INPUT_PULLUP);
  if (woke) {                                // the press that woke us must be released first
    rtc_gpio_deinit(PWR_BTN); rtc_gpio_deinit(TOUCH_INT); pinMode(PWR_BTN, INPUT_PULLUP);
  }
  xTaskCreatePinnedToCore(pwrGuard, "pwrGuard", 3072, NULL, 5, NULL, 0);
  Serial.begin(115200);
  power.POWEER_EPD_ON();
  power.POWEER_Audio_ON();
  i2c_master_Init();
  readInside();                             // before Wi-Fi and the CPU warm the case
  setenv("TZ", tzRule(), 1); tzset();      // v0.25.0: the zone is a setting
  rtcChipToSystem();
  flBoot(cause, pins, woke);                // field log: carry the run on, or settle the last one and open a new one
  wk(2);                                    // v0.26.1: awake
  if (!woke) touchReset();
  uint8_t v = 0;                            // touch chip: INT held low while touched,
  i2c_write_buff(touch_handle, 0xA4, &v, 1);             // low-power monitor mode when idle
  v = 1; i2c_write_buff(touch_handle, 0x86, &v, 1);
  v = 2; i2c_write_buff(touch_handle, 0x87, &v, 1);

  custom_lcd_spi_t cfg = {};
  cfg.cs = EPD_CS_PIN; cfg.dc = EPD_DC_PIN; cfg.rst = EPD_RST_PIN;
  cfg.busy = EPD_BUSY_PIN; cfg.mosi = EPD_MOSI_PIN; cfg.scl = EPD_SCK_PIN;
  cfg.spi_host = EPD_SPI_NUM; cfg.buffer_len = 5000;
  epd = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, cfg);

  if (woke) {                               // ---- back from deep sleep
    tile = rtcTile; onIdle = rtcIdle; jarAnswered = rtcJarAnswered;
    orAt = rtcOrAt; jarAt = rtcJarAt;
    orBody = rtcOr; jarBody = rtcJar;
    lastAnswer = rtcLast; lastGrammar = rtcLastGrammar;
    bool batEmpty = batWatch(wakeMv);       // reserve and empty are decided on the readings taken at a wake
    batSample();                            // after the saved forecast is back (outdoor temp)
    if (!onIdle && !lastAnswer.length()) onIdle = true;   // answer too long to keep: treat as idle
    if (rtcPaperKept) { epd->EPD_Init_Partial(); needFull = false; }   // panel still holds its image
    else { epd->EPD_Init(); needFull = true; }
    if (batEmpty) batteryEmptyOff();        // on battery this does not come back

    alarmLoad();
    if (ringIfDue()) { rtcRdOn = 0; rdOn = false; prefs.putBool("rdOn", false); goto awake; }
#if CLICKER
    if (rtcCkOn) {                          // ---- the Cl!cker was open
      if (nowS() - rtcCkLast > CK_SESSION_S || tile != T_CLICKER) ckEnd("left unused");   // a long sleep ends the session
      else if (cause == ESP_SLEEP_WAKEUP_TIMER) goSleep();   // a battery-log wake: nothing to show, the session goes on
      else { ckWake(pins); goto awake; }
    }
#endif
    if (cause == ESP_SLEEP_WAKEUP_TIMER && !rtcReserve && ksMorningDue()) {   // in reserve: no background fetching
      bool got = ksFetch();
      if (got && tile == T_KAFFEE && onIdle && !rtcRdOn) ksFace();
    }
    if (rtcRdOn) {                          // ---- the reader was open: offline all the way
      rdRestore(rtcRdSlug);
      rv = rtcRdView; rdCursor = rtcRdCursor; dkKind = rtcRdKind; dkSec = rtcRdSec; dkPos = rtcRdPos; revealed = rtcRdRev;
      if (rv == RV_LIBRARY && !edScanned) scanEditions();
      if (rv == RV_CARD) {
        buildDeck();
        if (!deckN) rv = RV_SECTIONS;
        else { layoutCard(deck[dkPos]); rdPage = constrain((int)rtcRdPage, 0, pgN - 1); }
      }
      if (cause == ESP_SLEEP_WAKEUP_TIMER) {           // 07:00: put the daily prompt on the paper
        if (potdWanted()) { showPotd(); rdDraw(); }
        goSleep();
      }
      audioEnsure();                       // ready for talking again if the user leaves the reader
      if (potdWanted()) { showPotd(); rdDraw(); goto awake; }
      if (pins & (1ULL << TOUCH_INT)) {
        int lx, ly;
        if (!touched() && lastLift(lx, ly)) {           // the tap that woke us, if the chip kept it
          rdPrime();
          int tx = TOUCH_FLIP_X ? 199 - lx : lx;
          rdEvent(tx < 67 ? EV_LEFT : tx > 133 ? EV_RIGHT : EV_MID, ly);
        } else rdDraw();                    // otherwise: just wake, same page
      } else if (pins & (1ULL << PWR_BTN)) {
        uint32_t t0 = millis();
        while (digitalRead(PWR_BTN) == LOW && millis() - t0 < OFF_MS) delay(20);
        if (millis() - t0 >= OFF_MS) powerOff(L("OFF", "\xC3\x89" "TEINT"));
        else rdDraw();
      } else rdDraw();
      goto awake;
    }

    if (cause == ESP_SLEEP_WAKEUP_TIMER && tile == T_CLOCK) {   // the minute tick
      bool due = !timeValid() || time(NULL) < (time_t)rtcNtpAt || time(NULL) - rtcNtpAt > NTP_EVERY_S
                 || !wxFresh();
      if (due && !rtcReserve && time(NULL) - rtcNetTryAt > WX_STALE_S) {       // online at most every 30 min, never in reserve
        rtcNetTryAt = time(NULL);
        wifiStart();
        wxFetchAll();
      }
      time_t e = time(NULL); struct tm t; localtime_r(&e, &t);
      if (t.tm_min == 0) needFull = true;   // hourly full refresh wipes partial-update ghosts
      epd->EPD_Clear(); tileDots(); drawClock(); show();
      goSleep();
    }
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {  // quiet refresh, then straight back to sleep
      bool due = onIdle && tile == T_ORACLE && !rtcReserve && nowS() - orAt >= ORACLE_STALE_S - 60;
      if (onIdle && tile == T_BATTERY) { epd->EPD_Clear(); tileDots(); drawBattery(); show(); }
      if (onIdle && tile == T_CLIMATE) {    // the station: a fresh forecast at most every 30 min, then the face
        if (!wxFresh() && !rtcReserve && time(NULL) - rtcNetTryAt > WX_STALE_S) { rtcNetTryAt = time(NULL); wifiStart(); wxFetchAll(); }
        if (!rtcReserve || reserveTurned) { epd->EPD_Clear(); tileDots(); climFace(); show(); }   // in reserve the 5-minute readings leave the paper alone
      }
      if (!due) goSleep();                  // a battery-log wake: sample taken, nothing else to do
      String b = callBrain(tileMode(tile), "application/json", (const uint8_t *)"{\"text\":\"\"}", 11);
      if (jsonField(b, "answer").length() && onIdle) {
        orBody = b; orAt = nowS();
        epd->EPD_Clear(); tileDots(); drawOracle(orBody); show();
      }
      goSleep();
    }

    audioEnsure();
    if (pins & (1ULL << TOUCH_INT)) {
      bool held = touched();
      uint8_t g = 0;                           // a swipe finished before we woke? ask the chip
      if (!held && i2c_read_buff(touch_handle, 0x01, &g, 1) == 0 && (g == 0x14 || g == 0x1C || g == 0x10 || g == 0x18)) {
        rtcSpurious = 0;
        swipe(g == 0x14 || g == 0x10 ? -1 : 1);   // left/up = next, right/down = previous
        goto awake;
      }
      if (!held && nowS() - rtcSleptAt < 3) {   // woke with no finger, right after sleeping:
        if (++rtcSpurious >= 3) rtcTouchWakeOff = 1;   // a stuck touch line; stop using it
      } else rtcSpurious = 0;
      int hx = 0, hy = 0;
      if (held && tile == T_DEVICE && onIdle && touchXY(hx, hy) && hy >= NL_Y) { waitRelease(); nlSet(!rtcNlOn, "by hand"); screenIdle(); }   // v0.25.5: the lamp, in one press
      else if (held && (tile == T_ASK || tile == T_DERDIEDAS || tile == T_JAR)) talk(true);
      else if (onIdle || !lastAnswer.length()) screenIdle();       // just wake: same tile, same screen
      else if (lastGrammar) screenGrammar(lastAnswer); else screenText(lastAnswer, lastAnswer.length() < 24);
    } else if (pins & (1ULL << PWR_BTN)) {
      uint32_t t0 = millis();
      while (digitalRead(PWR_BTN) == LOW && millis() - t0 < OFF_MS) delay(20);
      if (millis() - t0 >= OFF_MS) powerOff(L("OFF", "\xC3\x89" "TEINT"));
      else screenIdle();
    } else screenIdle();                    // BOOT or anything else: just show the face
    awake:;
  } else {                                  // ---- cold start
    rtcTouchWakeOff = 0; rtcSpurious = 0;
    alarmLoad();
    coldStart = true;                       // this reading is logged with its flag and not counted by the battery watch
    batSample();
    epd->EPD_Init(); epd->EPD_Clear();
    brandScreen(EK_VERSION, false);
    epd->EPD_DisplayPartBaseImage(); epd->EPD_Init_Partial(); needFull = false;
    audioEnsure();
    tile = prefs.getInt("tile", T_ASK);
    if (tilePos(tile) < 0) tile = TILES[0];
    prefs.putInt("sess", prefs.getInt("sess", 0) + 1);   // day counter for Leitner if the clock is unset
    rtcPotdDay = prefs.getInt("potdDay", 0);
    if (reading) {                          // was reading: straight back in, no Wi-Fi at all
      rdRestore(prefs.getString("rdEd", "edition-zero").c_str());
      if (catMode) { rv = RV_CATLIST; rdCursor = prefs.getInt("catLast", 0); }
      else { rv = RV_SPLASH; rdCursor = 0; }
      if (potdWanted()) showPotd();
      rdDraw();
    } else {
      if (!wifiConfigured()) { delay(1500); screenIdle(); }   // v0.24.0: nothing set up is not a failure. No search, no notice: the wordmark, then the tiles
      else {
        bool up = wifiUp();
        if (up) maybeSyncTime(); else rtcNetDownUntil = nowS() + NET_BACKOFF_S;
        String flNote = flNotice();         // empty unless the last run ended badly
        screenText((up ? L("connected.\n", "connect\xC3\xA9" ".\n") + String(netName[tryNet]) : L("no Wi-Fi", "pas de Wi-Fi") + wifiDiag()) + flNote, false);
        delay(up && !flNote.length() ? 900 : 6000);   // leave a failure up long enough to read
        screenIdle();
      }
    }
  }
  waitRelease();
  lastTouch = millis();
}

void loop() {
  userSession = true;                        // the loop only runs while awake for a person
  checkPower();
#if CLICKER
  if (ckOn) { ckLoop(); return; }
#endif
  flPoll(false);                             // BOOT button: a mark in the field log
  commitNtp();
  if (ringIfDue()) { clkOn = false; rdOn = false; }
#if CLIMATE
  if (cxOn) {                                 // climate log export pages
    if (millis() - lastTouch > RD_AWAKE_MS) { cxExit(); return; }
    if (!touched()) { delay(30); return; }
    cxTouch(); lastTouch = millis();
    return;
  }
#endif
  if (climOn) {                               // Clim@te pages: < > turn, middle refreshes (exports on the log page), hold goes back
    if (millis() - lastTouch > RD_AWAKE_MS) { climOn = false; screenIdle(); return; }
    if (!touched()) { delay(30); return; }
    int x = 0, y = 0; touchXY(x, y); uint32_t t0 = millis(); bool hold = false;
    while (touched()) { if (millis() - t0 >= 600) { hold = true; break; } delay(12); }
    waitRelease(); lastTouch = millis();
    if (hold) { climOn = false; screenIdle(); return; }
    int tx = TOUCH_FLIP_X ? 199 - x : x;
    if (tx >= 67 && tx < 133) { if (climPage == 0) cxEnter(); else climRefresh(); return; }
    int n = climPages(); climPage = (climPage + (tx < 67 ? n - 1 : 1)) % n; climDraw();
    return;
  }
  if (snOn) { snLoop(); return; }            // v0.25.3: the Sensors page
#if BATLOG
  if (bxOn) {                                 // battery log export pages
    if (millis() - lastTouch > RD_AWAKE_MS) { bxExit(); return; }
    if (!touched()) { delay(30); return; }
    bxTouch(); lastTouch = millis();
    return;
  }
#endif
  if (ksOn) {                                 // reading the edito
    if (millis() - lastTouch > RD_AWAKE_MS) { ksExit(); return; }
    if (!touched()) { delay(30); return; }
    ksTouch(); lastTouch = millis();
    return;
  }
  if (tile == T_KAFFEE && onIdle && ksSetUp() && !ksFresh() && !offlineNow()
      && time(NULL) - (time_t)rtcKsTry > 600) { if (ksFetch()) screenIdle(); }
  if (clkOn) {                               // alarm / timer pages
    if (millis() - lastTouch > RD_AWAKE_MS) { clkExit(); return; }
    static uint32_t lastSec = 0;             // a running timer counts down on screen
    if (clkPage == 1 && timerRunning() && (uint32_t)time(NULL) != lastSec) {
      uint32_t now = time(NULL), left = rtcTimerEnd - now;
      bool due = left <= 60 || (left % 60) == 0 || lastSec == 0;   // seconds at the end, minutes before
      lastSec = now;
      if (due) clkDraw();
    }
    if (!touched()) { delay(30); return; }
    clkTouch(); lastTouch = millis();
    return;
  }
  if (rdOn) {                                // reading: no mic, no network
    if (millis() - lastTouch > RD_AWAKE_MS) goSleep();
    if (!touched()) { delay(30); return; }
    rdTouch(); lastTouch = millis();
    return;
  }
  if (millis() - lastTouch > AWAKE_MS) goSleep();
  static int shownMin = -1;
  if (tile == T_CLOCK && onIdle) {
    time_t e = time(NULL); struct tm t; localtime_r(&e, &t);
    if (shownMin >= 0 && t.tm_min != shownMin) screenIdle();
    shownMin = t.tm_min;
  } else shownMin = -1;
  if (!touched()) { delay(30); return; }

  // pressed: tap, swipe or hold?
  int x0 = 0, y0 = 0, x = 0, y = 0;
  touchXY(x0, y0); x = x0; y = y0;
  uint32_t t0 = millis();
  bool moved = false, down = true;
  while (down) {
    int nx, ny;
    down = touchXY(nx, ny);
    if (down) { x = nx; y = ny; }
    if (abs(x - x0) > SWIPE_PX || abs(y - y0) > SWIPE_PX) moved = true;
    if (!moved && down && millis() - t0 >= HOLD_MS) break;   // still and held: talk
    delay(12);
  }
#if EK_INVISIBLEOCEAN
  if (down && !moved) {                     // a long hold (0.8 s) is an action here; shorter is a tap
    while (touched() && millis() - t0 < 800) delay(12);
    if (!touched()) down = false;
  }
  if (boxMode) {                            // welcome screen: tap = Wi-Fi setup, hold = start offline
    if (moved) { waitRelease(); lastTouch = millis(); return; }
    waitRelease();
    if (!down) wifiSetup(); else { unbox(); screenIdle(); }
    lastTouch = millis(); return;
  }
  if (down && !moved && (tile == T_CLIMATE || tile == T_RADIO || tile == T_DEVICE)) { waitRelease(); wifiSetup(); lastTouch = millis(); return; }
#endif
  if (moved) {                              // wait for the finger to lift, then decide
    while (touchXY(x, y)) delay(12);
    int dx = x - x0, dy = y - y0;
    swipe(abs(dx) >= abs(dy) ? dx : dy);
  } else if (!down) { tapY = y0; tap(x0); }
  else if (tile == T_ASK || tile == T_DERDIEDAS || tile == T_JAR) { if (wifiConfigured()) talk(false); }   // hold talks only here
  else if (tile == T_DEVICE) {                                                    // v0.25.0: a long hold opens the Wi-Fi and clock page
    while (touched() && millis() - t0 < 800) delay(12);
    if (touched()) { waitRelease(); wifiSetup(); }
  }
  else if (EK_FIELDLOG && tile == T_BATTERY) {                                   // a long hold here opens the field log
    while (touched() && millis() - t0 < 800) delay(12);
    if (touched()) flScreen();
  }
  waitRelease();                            // wait for a fresh touch
  lastTouch = millis();
}

// ---------------------------------------------------------------- built-in packs
// generated from editions/<slug>/*.json (the same files work on a microSD card)
const char GU_EDITION[] =
  "{\n"
  " \"name\": \"Guineapedi@ Lite\",\n"
  " \"slug\": \"guineapedia-lite\",\n"
  " \"version\": \"1.0.2\",\n"
  " \"lang\": \"en\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"family\": \"guineapedia-lite\",\n"
  " \"sources\": [\n"
  "  \"MSD Veterinary Manual, pet owner version: Guinea Pigs (description and physical characteristics; nutritional problems). https://www.msdvetmanual.com/all-other-pets/guinea-pigs\",\n"
  "  \"MSD Veterinary Manual, professional version: Housing and Nutrition of Guinea Pigs.\",\n"
  "  \"Merck Veterinary Manual: Physiological Data for Guinea Pigs (table).\",\n"
  "  \"Royal Veterinary College, Beaumont Sainsbury Animal Hospital (2020). Guinea pig care tips.\",\n"
  "  \"Royal Veterinary College, Beaumont Sainsbury Animal Hospital (2020). Guinea pig feeding guidelines.\",\n"
  "  \"PDSA: Keeping your guinea pigs cool in the summer, dos and don'ts.\",\n"
  "  \"PetMD: GI stasis in guinea pigs.\",\n"
  "  \"RSPCA Australia knowledge base: How do guinea pigs breed?\",\n"
  "  \"Woodnutt J. (MRCVS), PetsRadar: Guinea pig noises and body language explained.\",\n"
  "  \"Hoefer H. L. (2001). Common problems in guinea pigs. Atlantic Coast Veterinary Conference (VIN).\",\n"
  "  \"Spotorno A. E. et al. (2006). Ancient and modern steps during the domestication of guinea pigs (Cavia porcellus L.). Journal of Zoology 270(1), 57-62.\",\n"
  "  \"FOUR PAWS: Guinea pigs, their history.\",\n"
  "  \"swissinfo.ch: fact check on lonely guinea pigs and the Swiss Animal Protection Ordinance (in force since September 2008).\",\n"
  "  \"The Royal Mint: The story of the guinea.\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"body\",\n"
  "   \"title\": \"Body and senses\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"talk\",\n"
  "   \"title\": \"Sounds and signs\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"food\",\n"
  "   \"title\": \"Food and vitamin C\",\n"
  "   \"order\": 3\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"home\",\n"
  "   \"title\": \"Home and company\",\n"
  "   \"order\": 4\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"health\",\n"
  "   \"title\": \"When to worry\",\n"
  "   \"order\": 5\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"sitter\",\n"
  "   \"title\": \"Pig-sitting\",\n"
  "   \"order\": 6\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"history\",\n"
  "   \"title\": \"History and names\",\n"
  "   \"order\": 7\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"more\",\n"
  "   \"title\": \"More\",\n"
  "   \"order\": 8\n"
  "  }\n"
  " ]\n"
  "}\n";
const char GU_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"not-pigs\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Not pigs, not Guinea\",\n"
  "  \"body\": \"Guinea pigs are rodents of the cavy family, from South America. The domestic species, **Cavia porcellus**, does not exist in the wild: its wild relatives live on grassy plains and eat grasses and leaves.\\n\\nSo: not a pig, and not from Guinea. Where the name comes from is told in History and names.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"teeth\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Teeth that never stop\",\n"
  "  \"body\": \"A guinea pig has 20 teeth, and all of them are open-rooted: they keep growing for the whole of its life. Chewing is what keeps them at the right length, and hay is the best tool for the job.\\n\\nToo little hay, with too many pellets or treats, is the classic road to dental disease.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"eyes\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Eyes on the sides\",\n"
  "  \"body\": \"The eyes sit on the sides of the head. That gives a wide view all around, good for spotting a predator, but a poor view straight ahead.\\n\\nA hand arriving from the front can come as a surprise. Move slowly, and let them hear you before they see you.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"feet\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Four toes, three toes\",\n"
  "  \"body\": \"Each front foot has four toes and each back foot three. There is no visible tail.\\n\\nThey dislike open spaces and feel safest under cover: a tunnel or a box means more to them than a big empty floor.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"numbers\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The numbers\",\n"
  "  \"body\": \"Adult weight: 700 to 900 g for a sow, 900 to 1,200 g for a boar.\\nBody temperature: 38 to 39.5 °C.\\nHeart: 200 to 350 beats a minute.\\nBreathing: 40 to 100 breaths a minute.\\nWater: about 100 ml per kilogram of body weight a day.\\nLifespan: 5 to 8 years.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hindgut\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"A gut built for grass\",\n"
  "  \"body\": \"Guinea pigs are strict plant eaters with a long hindgut, where microbes ferment the fibre. Like rabbits, they eat some of their own soft droppings every day, straight from the bottom. This is normal: it lets the food pass through twice.\\n\\nFibre keeps the whole gut moving. Without enough of it, the gut slows down, or stops.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-teeth\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Why hay first?\",\n"
  "  \"question\": \"Why must there always be hay in the cage?\",\n"
  "  \"answer\": \"Their teeth grow all their life and hay wears them down. Its fibre also keeps the gut moving.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-weight\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Sow or boar?\",\n"
  "  \"question\": \"Which is usually heavier, an adult sow or an adult boar?\",\n"
  "  \"answer\": \"The boar: about 900 to 1,200 g, against 700 to 900 g for a sow.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-droppings\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Eating droppings\",\n"
  "  \"question\": \"You see a guinea pig eating from its own bottom. Should you worry?\",\n"
  "  \"answer\": \"No. It eats soft droppings every day, like a rabbit, so its food passes through the gut twice.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wheek\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The wheek\",\n"
  "  \"body\": \"A loud, slightly demanding whistle, aimed mostly at humans. It asks for food, for attention, or for a run outside.\\n\\nMost guinea pigs quickly learn who brings the vegetables.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"purr\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Purr or rumble\",\n"
  "  \"body\": \"A soft, bubbling purr with a relaxed body means a content guinea pig. The same sound in short bursts, with a tense body, means annoyance.\\n\\nA deeper rumble, with swaying and strutting, is a show of dominance, or a male courting. Some rumble when touched where they dislike it, often near the bottom.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"chatter\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Teeth chattering\",\n"
  "  \"body\": \"Chattering teeth mean anger or annoyance: back off. Lifting the chin high, or yawning to show the teeth, says the same.\\n\\nChattering can also point to tooth trouble or pain, so a pig that chatters for no clear reason deserves a closer look. It is not the happy crunching of hay.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"popcorn\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Popcorning\",\n"
  "  \"body\": \"A sudden jump in the air, a twist, a hop, then a dash: this is popcorning, and it means excitement and happiness.\\n\\nYoung guinea pigs do it most, but adults can popcorn too.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"chirp\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The chirp\",\n"
  "  \"body\": \"Chirping is rare and still poorly understood. It is thought to be a sign of stress, and it is more common in young guinea pigs.\\n\\nIf you hear it, look at what changed around them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wheek\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"The whistle\",\n"
  "  \"question\": \"A guinea pig whistles loudly as you walk in. What is it most likely asking for?\",\n"
  "  \"answer\": \"Food, or your attention. The wheek is aimed mostly at humans.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-chatter\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Chattering\",\n"
  "  \"question\": \"What do chattering teeth usually mean?\",\n"
  "  \"answer\": \"Anger or annoyance: back off. If it happens for no reason, think of pain or tooth trouble.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-popcorn\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Jumping for joy\",\n"
  "  \"question\": \"A young guinea pig leaps and twists in the air. Good sign or bad sign?\",\n"
  "  \"answer\": \"Good: that is popcorning, a sign of excitement and happiness.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-purr\",\n"
  "  \"section\": \"talk\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Two purrs\",\n"
  "  \"question\": \"The same purr can mean two things. What tells them apart?\",\n"
  "  \"answer\": \"The body. Relaxed, with a long purr: content. Tense, with short bursts: annoyed.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"vit-c\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"No vitamin C factory\",\n"
  "  \"body\": \"Like humans, guinea pigs cannot make their own vitamin C, so it must come from food every day. An adult needs at least 10 mg per kilogram of body weight per day, a pregnant sow about 30 mg.\\n\\nMost healthy guinea pigs get enough from fresh vegetables and good pellets.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"scurvy\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Scurvy\",\n"
  "  \"body\": \"Without vitamin C, guinea pigs get scurvy, as sailors once did. Signs include a rough coat, loss of appetite, swollen joints and lameness, weight loss, diarrhoea, bleeding under the skin and tooth problems.\\n\\nScurvy can kill. It is a job for the vet.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"fragile-c\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"A fragile vitamin\",\n"
  "  \"body\": \"Vitamin C breaks down easily. In pellets, heat, light and moisture destroy it, and stored food can lose much of it within a few months. In water it breaks down too, and it binds to metal, so drops in the drinking bottle are unreliable.\\n\\nIf a vet asks for a supplement, a pinch of crushed tablet on the food works better.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"plate\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The plate\",\n"
  "  \"body\": \"Hay or grass: at least 80% of the diet, and it never runs out.\\nVegetables, weeds and herbs: 10 to 15%, with variety.\\nPellets: 5 to 10%, measured, never a free buffet.\\nFruit and carrots: small treats, once or twice a week.\\nMuesli mixes: never.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"c-veg\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Greens rich in C\",\n"
  "  \"body\": \"Red and green bell peppers are among the best sources of vitamin C. Broccoli, cabbage, romaine lettuce and herbs such as coriander, dill and mint are good choices too.\\n\\nKeep calcium-rich greens such as parsley, spinach and kale to small amounts, and choose grass hay over alfalfa, which is high in calcium.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"never\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Never on the menu\",\n"
  "  \"body\": \"Tomato plants and leaves (the ripe fruit is fine), potato plants, rhubarb, ivy, buttercup, foxglove, lily of the valley and yew are poisonous. Many other garden and wild plants are too: feed only what you can name with certainty.\\n\\nNuts, seeds and sugary treats do harm as well. Iceberg lettuce is not on the safe list.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"water\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Water\",\n"
  "  \"body\": \"Fresh water every day, in a clean bottle or bowl. Check that the bottle actually works: a blocked spout looks full.\\n\\nIn hot weather water warms up fast. Change it more often, and offer more than one source.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-vitc\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Why every day?\",\n"
  "  \"question\": \"Why does a guinea pig need vitamin C in its food every day?\",\n"
  "  \"answer\": \"It cannot make its own, like humans. Without it, scurvy sets in.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-plate\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"The biggest part\",\n"
  "  \"question\": \"What should make up at least 80% of the diet?\",\n"
  "  \"answer\": \"Hay or grass.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-bottle\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Drops in the bottle\",\n"
  "  \"question\": \"Why are vitamin C drops in the water bottle a poor idea?\",\n"
  "  \"answer\": \"Vitamin C breaks down in water and light, and binds to metal. A crushed tablet on the food works better.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-tomato\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Tomato, yes or no?\",\n"
  "  \"question\": \"Can a guinea pig eat tomato?\",\n"
  "  \"answer\": \"The ripe fruit, yes. The plant and its leaves are poisonous.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-fruit\",\n"
  "  \"section\": \"food\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Fruit\",\n"
  "  \"question\": \"How often can a guinea pig have fruit?\",\n"
  "  \"answer\": \"As a small treat, once or twice a week, because of the sugar.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"company\",\n"
  "  \"section\": \"home\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Never alone\",\n"
  "  \"body\": \"Guinea pigs are social and prefer to live in groups. Swiss law has said so since September 2008: guinea pigs must be kept in groups of at least two.\\n\\nIn a mixed group, only one male should be left intact, since males fight over females.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"rabbits\",\n"
  "  \"section\": \"home\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Not with rabbits\",\n"
  "  \"body\": \"Rabbits and guinea pigs make poor housemates. Rabbits can carry **Bordetella bronchiseptica**, a bacterium that can cause fatal pneumonia in guinea pigs. Rabbits can also bully them, and the two eat differently.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"space\",\n"
  "  \"section\": \"home\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Room to roam\",\n"
  "  \"body\": \"For a pair, the floor should be at least 120 by 60 cm, and bigger is better. Use a solid floor, never wire, with deep hay or soft bedding, and avoid pine and cedar shavings.\\n\\nGive them more hiding places than there are guinea pigs: they dislike open spaces.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"temperature\",\n"
  "  \"section\": \"home\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Neither hot nor cold\",\n"
  "  \"body\": \"Guinea pigs are comfortable at about 18 to 24 °C, and should never be kept above 26 °C. Overheating is very dangerous for them.\\n\\nKeep the cage out of direct sunlight, and in a quiet spot away from dogs and cats.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hot-day\",\n"
  "  \"section\": \"home\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"On a hot day\",\n"
  "  \"items\": [\n"
  "   \"Cage out of direct sun\",\n"
  "   \"Curtains closed in the room\",\n"
  "   \"Several water sources, changed often\",\n"
  "   \"Wooden house or a cool tile to lie on\",\n"
  "   \"Frozen bottle in a towel, outside the cage\",\n"
  "   \"Watery food: celery, cucumber\",\n"
  "   \"No cold baths: they cause shock\",\n"
  "   \"Long coats brushed\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-swiss\",\n"
  "  \"section\": \"home\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Swiss law\",\n"
  "  \"question\": \"Since 2008, what does Swiss law say about keeping guinea pigs?\",\n"
  "  \"answer\": \"They must be kept in groups of at least two: a guinea pig may not live alone.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-rabbit\",\n"
  "  \"section\": \"home\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Rabbit housemate?\",\n"
  "  \"question\": \"Why not house a guinea pig with a rabbit?\",\n"
  "  \"answer\": \"Rabbits can carry Bordetella, deadly to guinea pigs. They can also bully them, and they eat differently.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-heat\",\n"
  "  \"section\": \"home\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Too warm\",\n"
  "  \"question\": \"Above what temperature does it get risky for guinea pigs?\",\n"
  "  \"answer\": \"26 °C. They are comfortable at about 18 to 24 °C.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"not-eating\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Not eating is urgent\",\n"
  "  \"body\": \"A guinea pig that stops eating needs a vet the same day. The gut can slow down or stop (gut stasis), and without treatment this can turn fatal within 24 to 48 hours.\\n\\nWatch for fewer, smaller or no droppings, a hunched posture, a hard belly and grinding teeth.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"heatstroke\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Heatstroke\",\n"
  "  \"body\": \"Signs: fast, shallow breathing, drooling, unusual sleepiness, collapse or fits.\\n\\nMove the guinea pig somewhere cool and call a vet at once. Never put it in cold water: the shock can do more harm than the heat.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breathing\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Sneezes and wee\",\n"
  "  \"body\": \"Sneezing, a runny nose or noisy breathing can mean a respiratory infection, and pneumonia is hard to spot in guinea pigs.\\n\\nBlood in the urine, straining, or no urine at all points to the bladder or kidneys. Both are vet visits, not wait-and-see.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"boars\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Boars and bottoms\",\n"
  "  \"body\": \"Intact males, older ones above all, can collect droppings at the back end (impaction). Vets advise checking them daily, because a dirty rear attracts flies.\\n\\nAn annual check-up catches most problems while they are still easy to treat.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"vet-now\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Call the vet today\",\n"
  "  \"items\": [\n"
  "   \"Not eating, or not drinking\",\n"
  "   \"Few, small or no droppings\",\n"
  "   \"Hunched, puffed up, not moving\",\n"
  "   \"Fast breathing or wheezing\",\n"
  "   \"Blood in the urine, or straining\",\n"
  "   \"Drooling\",\n"
  "   \"Grinding teeth\",\n"
  "   \"Sudden lameness\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-stasis\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Not eating\",\n"
  "  \"question\": \"A guinea pig has not eaten all morning. Wait until tomorrow?\",\n"
  "  \"answer\": \"No. Call a vet today: gut stasis can turn fatal within 24 to 48 hours.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-heatstroke\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Cold bath?\",\n"
  "  \"question\": \"First aid for an overheated guinea pig: a cold bath?\",\n"
  "  \"answer\": \"No. Cold water can cause shock. Move it somewhere cool and call the vet.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-poops\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Fewer droppings\",\n"
  "  \"question\": \"Fewer and smaller droppings than usual: what can it mean?\",\n"
  "  \"answer\": \"The gut is slowing down. With less eating, it is a reason to call the vet.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-msd\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"MSD Vet Manual\",\n"
  "  \"text\": \"The MSD Veterinary Manual's pages on guinea pigs, written for owners: diet, routine care and health problems.\",\n"
  "  \"url\": \"https://www.msdvetmanual.com/all-other-pets/guinea-pigs\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"handover\",\n"
  "  \"section\": \"sitter\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Before they leave\",\n"
  "  \"body\": \"When friends leave their guinea pigs with you, their routine matters more than any book. Ask to watch one full feeding, write the amounts down, and keep the same foods and the same times.\\n\\nAgree in advance on who to call, and who pays, if a vet is needed.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"ask-first\",\n"
  "  \"section\": \"sitter\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Ask before they go\",\n"
  "  \"items\": [\n"
  "   \"Vet's name, phone and opening hours\",\n"
  "   \"Emergency vet for nights and weekends\",\n"
  "   \"Each pig's name, and how to tell them apart\",\n"
  "   \"Food brands and daily amounts\",\n"
  "   \"Usual weights, if they weigh them\",\n"
  "   \"Treats allowed, and how often\",\n"
  "   \"Who pays the vet, and up to how much\",\n"
  "   \"Where the spare hay and bedding are\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"daily-round\",\n"
  "  \"section\": \"sitter\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"The daily round\",\n"
  "  \"items\": [\n"
  "   \"Hay topped up, more than enough\",\n"
  "   \"Fresh water, bottle working\",\n"
  "   \"Vegetables, as the owners give them\",\n"
  "   \"Pellets, measured\",\n"
  "   \"Every pig seen eating\",\n"
  "   \"Droppings: normal number and size\",\n"
  "   \"Wet or dirty spots cleaned\",\n"
  "   \"A look at eyes, nose and bottom\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"counting\",\n"
  "  \"section\": \"sitter\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Count pigs and poops\",\n"
  "  \"body\": \"Two weeks is long enough for a problem to start, and short enough to miss it. Each day, make sure every guinea pig comes to the food, and that the droppings stay plentiful and well formed.\\n\\nAn empty bowl tells you little: one pig may have eaten for two.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"weigh\",\n"
  "  \"section\": \"sitter\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The kitchen scale\",\n"
  "  \"body\": \"If the owners weigh their guinea pigs, keep doing it: same day each week, same scale, write it down.\\n\\nFur hides weight loss well. A steady drop is a reason to call the vet, even if the pig still looks round.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-bowl\",\n"
  "  \"section\": \"sitter\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"The empty bowl\",\n"
  "  \"question\": \"The bowl is empty every evening. Proof that both guinea pigs are eating?\",\n"
  "  \"answer\": \"No. One may eat for two. Watch each pig come to the food.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-number\",\n"
  "  \"section\": \"sitter\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"The number to have\",\n"
  "  \"question\": \"Which phone numbers should you have before the owners leave?\",\n"
  "  \"answer\": \"Their vet's, and an emergency vet for nights and weekends.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"andes\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"From the Andes\",\n"
  "  \"body\": \"People in the Andes, in today's Peru, Bolivia and Ecuador, began domesticating guinea pigs around 5000 BCE, probably from a single first domestication in the western Andes.\\n\\nThey were kept mainly for food, and they turn up in local folk art from 500 BCE to 500 CE.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"moche\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Gods and healers\",\n"
  "  \"body\": \"The Moche of northern Peru, around 100 to 800 CE, showed guinea pigs often in their art. The Incas offered them to their gods.\\n\\nSome Peruvians believed guinea pigs could help diagnose illness, black ones above all.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"europe\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"To Europe\",\n"
  "  \"body\": \"Spanish traders brought guinea pigs to Europe in the 16th century, where they became exotic pets for the rich and for royal courts. Queen Elizabeth I of England is said to have been one of their first European fans.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"names\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"What's in a name\",\n"
  "  \"body\": \"One story says guinea pigs sold for a guinea each in England. But the guinea coin was first struck in 1663, a century after they arrived, so nobody really knows.\\n\\nThe pig is easier. The Latin **porcellus** means little pig, and the German **Meerschweinchen**, little sea pig, may owe its pig to their squeaks.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breeds\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Many coats\",\n"
  "  \"body\": \"There are at least 13 recognised breeds, with short, long, curly or rosette coats, and hairless varieties as well.\\n\\nLong-haired guinea pigs need regular brushing, and more in summer.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"pups\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Born ready\",\n"
  "  \"body\": \"Pregnancy lasts 59 to 72 days, long for a rodent. Pups are born furred, with their eyes open and their teeth already through, and they are on their feet within hours.\\n\\nA litter is usually 2 to 4 pups.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-pups\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Newborns\",\n"
  "  \"question\": \"How are guinea pig pups born: blind and naked, or furred with eyes open?\",\n"
  "  \"answer\": \"Furred, eyes open, teeth through, and walking within hours.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-andes\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Where from?\",\n"
  "  \"question\": \"Where were guinea pigs first domesticated, and roughly when?\",\n"
  "  \"answer\": \"In the Andes of South America, around 5000 BCE.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-guinea\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"A guinea each?\",\n"
  "  \"question\": \"Why is the story that guinea pigs cost one guinea doubtful?\",\n"
  "  \"answer\": \"The guinea coin was first struck in 1663, a century after guinea pigs reached England.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-catlas\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"C@tlas\",\n"
  "  \"text\": \"The first animal edition: facts and science about cats, on the E!nkster Editions station.\",\n"
  "  \"url\": \"https://einkster-editions.netlify.app/#/e/catlas\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-station\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"E!nkster Editions\",\n"
  "  \"text\": \"All the editions, free, offline once opened, no account.\",\n"
  "  \"url\": \"https://einkster-editions.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Guineapedi@ Lite\",\n"
  "  \"body\": \"Written by Julien and Claude the AI\"\n"
  " }\n"
  "]\n";
const char EZ_EDITION[] =
  "{\n"
  " \"name\": \"Edit!on Zero\",\n"
  " \"slug\": \"edition-zero\",\n"
  " \"version\": \"1.0.1\",\n"
  " \"lang\": \"en\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"sources\": [\n"
  "  \"Einkster firmware v0.10.0\",\n"
  "  \"Waveshare ESP32-S3-Touch-ePaper-1.54 (V2) documentation\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"start\",\n"
  "   \"title\": \"Start here\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"inside\",\n"
  "   \"title\": \"Inside the device\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"practice\",\n"
  "   \"title\": \"Practice\",\n"
  "   \"order\": 3\n"
  "  }\n"
  " ]\n"
  "}\n";
const char EZ_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"what\",\n"
  "  \"section\": \"start\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"What this is\",\n"
  "  \"body\": \"An **edition** is a pack of cards you read on paper. It works fully offline: no Wi-Fi, no relay, no keys.\\n\\nTalking to Claude lives on the other tiles and is not touched by reading.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"move\",\n"
  "  \"section\": \"start\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"How to move\",\n"
  "  \"body\": \"**Right third**: next page.\\n**Left third**: back.\\n**Middle**: act on the card (reveal, tick).\\n**Hold**: up one level.\\n**PWR**: back to the tiles.\\nSwipes turn pages too.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"first-steps\",\n"
  "  \"section\": \"start\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"First steps\",\n"
  "  \"items\": [\n"
  "   \"Turn a page with the right third\",\n"
  "   \"Tick this box with a tap on it\",\n"
  "   \"Reveal a prompt in Practice\",\n"
  "   \"Switch prompt of the day on\",\n"
  "   \"Switch off and on: ticks stay\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"station\",\n"
  "  \"section\": \"start\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"The kitchen\",\n"
  "  \"text\": \"Einkster is the starter on the InvisibleChaos DIY menu: build notes, case files and the pack spec.\",\n"
  "  \"url\": \"https://ic-kitchen.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"board\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The board\",\n"
  "  \"body\": \"ESP32-S3, 8 MB flash, 8 MB PSRAM.\\n200 × 200 e-paper with touch.\\nMicrophone and speaker (ES8311).\\nRoom sensor (SHTC3): ±0.2 °C.\\nClock chip (PCF85063).\\nmicroSD slot for editions.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"paper\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Paper physics\",\n"
  "  \"body\": \"Each pixel is a tiny capsule of black and white particles floating in clear fluid. A voltage pulls one colour to the surface; when the pulse stops, the particles stay where they are. That is why the page keeps its image at **zero power**, for days, even with the battery removed.\\n\\nMoving particles is slow, so a quick **partial refresh** only nudges the pixels that change. Some particles lag behind and leave a faint ghost of the previous page. Every few turns Einkster runs a **full refresh**: the screen flashes black and white a few times and every capsule starts from a clean state.\\n\\nCold makes the fluid thicker. Below about 0 °C refreshes get slow and washed out; the usual range is 0 to 50 °C.\\n\\nReading costs energy only when the page changes, which is why an e-paper reader can last weeks on a battery that a phone empties in a day.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"lexique\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Petit lexique\",\n"
  "  \"body\": \"L'écran garde l'image sans énergie. Ça coûte ≈ 0 µW à l'arrêt.\\n**Symboles** : masse volumique ρ, écart type σ, variation Δt, salinité 35 ‰, CO₂, H₂O, 3 m², 2 m³, ±0,5 °C, 4 × 4, « guillemets », cœur, maïs, sûr, forêt.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-ghost\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Ghosts\",\n"
  "  \"question\": \"Why does the paper blink black and white now and then?\",\n"
  "  \"answer\": \"A full refresh wipes the faint ghosts that partial refreshes leave behind.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-leitner\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Five boxes\",\n"
  "  \"question\": \"What happens to a prompt you get right?\",\n"
  "  \"answer\": \"It moves up one of five boxes and comes back less often: after 1, 2, 4, 8, then 16 days. A miss sends it back to box 1.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-permille\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Per mille\",\n"
  "  \"question\": \"What does 35 ‰ mean for seawater?\",\n"
  "  \"answer\": \"35 grams of salt per kilogram of water: parts per thousand.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Edit!on Zero\",\n"
  "  \"body\": \"Written by Julien and Claude the AI\"\n"
  " }\n"
  "]\n";
const char AT_EDITION[] =
  "{\n"
  " \"name\": \"Atlantid!a Lite\",\n"
  " \"slug\": \"atlantidia-lite\",\n"
  " \"version\": \"1.0.1\",\n"
  " \"lang\": \"fr\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"sources\": [\n"
  "  \"IOC, SCOR, IAPSO (2010). The international thermodynamic equation of seawater 2010 (TEOS-10). IOC-UNESCO, Manuals and Guides 56.\",\n"
  "  \"Fox-Kemper, B. et al. (2021). Ocean, Cryosphere and Sea Level Change. In: Climate Change 2021, IPCC AR6 WG1, chapter 9.\",\n"
  "  \"Talley, L. D., Pickard, G. L., Emery, W. J., Swift, J. H. (2011). Descriptive Physical Oceanography, 6th ed. Academic Press.\",\n"
  "  \"Stewart, R. H. (2008). Introduction to Physical Oceanography. Texas A&M University, open textbook.\",\n"
  "  \"Argo (2000). Argo float data and metadata from Global Data Assembly Centre (Argo GDAC). SEANOE, doi:10.17882/42182.\",\n"
  "  \"E.U. Copernicus Marine Service, Global Ocean Physics Analysis and Forecast, doi:10.48670/moi-00016.\",\n"
  "  \"NOAA National Ocean Service, Tides and Currents educational resources.\",\n"
  "  \"Friedlingstein, P. et al. (2023). Global Carbon Budget 2023. Earth System Science Data 15.\",\n"
  "  \"Paulmier, A., Ruiz-Pino, D. (2009). Oxygen minimum zones (OMZs) in the modern ocean. Progress in Oceanography 80.\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"eau-de-mer\",\n"
  "   \"title\": \"L’eau de mer\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"circulation\",\n"
  "   \"title\": \"Circulation\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"marees-vagues\",\n"
  "   \"title\": \"Marées et vagues\",\n"
  "   \"order\": 3\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"vie-marine\",\n"
  "   \"title\": \"Vie marine\",\n"
  "   \"order\": 4\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"mesurer\",\n"
  "   \"title\": \"Mesurer l’océan\",\n"
  "   \"order\": 5\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"climat\",\n"
  "   \"title\": \"Océan et climat\",\n"
  "   \"order\": 6\n"
  "  }\n"
  " ]\n"
  "}\n";
const char AT_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"salinite\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Salinité\",\n"
  "  \"body\": \"L’eau de mer contient en moyenne environ **35 g** de sels dissous par kilogramme. Au large, la salinité varie surtout entre 33 et 37.\\n\\nElle monte là où l’évaporation domine (mer Rouge, près de 40) et baisse là où arrivent pluie, fleuves ou glace fondue (Baltique, souvent moins de 10).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"densite\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Densité\",\n"
  "  \"body\": \"La densité de l’eau de mer en surface vaut en général entre **1020 et 1029 kg/m³**.\\n\\nElle augmente quand l’eau se refroidit, quand elle devient plus salée et, un peu, avec la pression. Une eau plus dense s’enfonce sous une eau plus légère : c’est le moteur de la circulation profonde.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"gel\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Point de congélation\",\n"
  "  \"body\": \"Le sel abaisse le point de congélation : une eau de mer à 35 gèle vers **−1,9 °C**, pas à 0 °C.\\n\\nContrairement à l’eau douce, dont la densité est maximale vers 4 °C, l’eau de mer devient de plus en plus dense en refroidissant, jusqu’à geler. En surface, la banquise rejette l’essentiel de son sel.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"pression\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Pression\",\n"
  "  \"body\": \"La pression augmente d’environ **1 dbar par mètre** de profondeur, soit à peu près une atmosphère tous les 10 m.\\n\\nC’est pratique : en océanographie, la pression en décibars donne presque directement la profondeur en mètres. À 4000 m, la pression vaut environ 400 fois celle de l’air en surface.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"thermocline\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Thermocline\",\n"
  "  \"body\": \"Sous la **couche de mélange**, brassée par le vent et bien homogène, la température chute rapidement sur quelques centaines de mètres : c’est la thermocline.\\n\\nEn dessous, l’océan profond est froid et stable, souvent entre 0 et 4 °C. La thermocline agit comme un couvercle qui limite les échanges entre surface et profondeur.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"diagramme-ts\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Diagramme T-S\",\n"
  "  \"body\": \"On trace la température en fonction de la salinité pour chaque profondeur d’un profil. Les lignes d’égale densité (**isopycnes**) s’y dessinent en courbes.\\n\\nChaque masse d’eau garde une signature T-S acquise en surface, là où elle s’est formée : le diagramme permet de les reconnaître et de voir leurs mélanges.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"son\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Vitesse du son\",\n"
  "  \"body\": \"Le son voyage dans l’eau de mer à environ **1500 m/s**, plus de quatre fois plus vite que dans l’air.\\n\\nSa vitesse augmente avec la température, la salinité et la pression. Vers 1000 m aux latitudes moyennes, elle passe par un minimum : ce **chenal SOFAR** guide le son sur des milliers de kilomètres.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-densite\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Densité\",\n"
  "  \"question\": \"Qu’est-ce qui rend une eau de mer plus dense ?\",\n"
  "  \"answer\": \"Une température plus basse, une salinité plus élevée et, dans une moindre mesure, la pression.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-gel\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Congélation\",\n"
  "  \"question\": \"Vers quelle température gèle une eau de mer de salinité 35 ?\",\n"
  "  \"answer\": \"Vers −1,9 °C : le sel abaisse le point de congélation.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-dbar\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Pression\",\n"
  "  \"question\": \"Une sonde indique 500 dbar. À quelle profondeur est-elle environ ?\",\n"
  "  \"answer\": \"Vers 500 m : la pression augmente d’environ 1 dbar par mètre.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-conductivite\",\n"
  "  \"section\": \"eau-de-mer\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Salinité\",\n"
  "  \"question\": \"Quelle grandeur mesure-t-on en pratique pour calculer la salinité ?\",\n"
  "  \"answer\": \"La conductivité électrique, avec la température et la pression. Les proportions constantes des ions le permettent.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"gyres\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Gyres\",\n"
  "  \"body\": \"Les vents dominants entraînent de grands tourbillons de surface, les **gyres**, un dans chaque bassin subtropical : Atlantique Nord et Sud, Pacifique Nord et Sud, océan Indien.\\n\\nIls tournent dans le sens des aiguilles d’une montre dans l’hémisphère Nord, en sens inverse dans l’hémisphère Sud.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bord-ouest\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Courants de bord ouest\",\n"
  "  \"body\": \"Les gyres sont plus rapides et plus étroits sur leur bord ouest : Gulf Stream, Kuroshio, courant du Brésil, des Aiguilles.\\n\\nCette **intensification ouest** vient de la rotation de la Terre et de la variation de l’effet de Coriolis avec la latitude. Le Gulf Stream peut dépasser 2 m/s en surface.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"ekman\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Transport d’Ekman\",\n"
  "  \"body\": \"Le vent entraîne la couche de surface, mais la force de Coriolis la dévie. En moyenne sur la couche, l’eau part à **90° à droite** du vent dans l’hémisphère Nord, à gauche dans l’hémisphère Sud.\\n\\nAvec la profondeur, le courant tourne et faiblit : c’est la spirale d’Ekman.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"upwelling\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Upwelling\",\n"
  "  \"body\": \"Le long de certaines côtes (Pérou, Californie, Canaries, Benguela), le vent parallèle à la côte pousse l’eau de surface vers le large par transport d’Ekman.\\n\\nDe l’eau froide et riche en nutriments remonte la remplacer. Ces zones d’**upwelling** couvrent une petite part de l’océan, mais nourrissent une grande part des pêches mondiales.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"thermohaline\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Circulation profonde\",\n"
  "  \"body\": \"Dans les mers nordiques, la mer du Labrador et autour de l’Antarctique, l’eau de surface devient assez froide et salée pour plonger vers le fond.\\n\\nElle alimente une circulation lente à l’échelle du globe, dite **thermohaline**. Un tour complet prend de l’ordre d’un millier d’années.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"el-nino\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"El Niño\",\n"
  "  \"body\": \"Normalement, les alizés poussent l’eau chaude vers l’ouest du Pacifique équatorial et l’upwelling refroidit l’est.\\n\\nPendant **El Niño**, les alizés faiblissent, l’eau chaude s’étend vers l’est et l’upwelling ne remonte plus d’eau froide au large du Pérou. Les pluies du globe entier s’en trouvent déplacées. La Niña en est l’inverse.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-ekman\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Ekman\",\n"
  "  \"question\": \"Dans l’hémisphère Nord, dans quelle direction part en moyenne l’eau poussée par le vent ?\",\n"
  "  \"answer\": \"À 90° à droite de la direction du vent, en moyenne sur la couche d’Ekman.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-bord-ouest\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Gyres\",\n"
  "  \"question\": \"Pourquoi le Gulf Stream est-il plus rapide et plus étroit que le courant des Canaries ?\",\n"
  "  \"answer\": \"C’est l’intensification des courants de bord ouest, due à la variation de l’effet de Coriolis avec la latitude.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-upwelling\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Upwelling\",\n"
  "  \"question\": \"Pourquoi les zones d’upwelling sont-elles si poissonneuses ?\",\n"
  "  \"answer\": \"L’eau profonde qui remonte apporte des nutriments en surface, où la lumière permet au phytoplancton de proliférer.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-el-nino\",\n"
  "  \"section\": \"circulation\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"El Niño\",\n"
  "  \"question\": \"Pendant El Niño, qu’arrive-t-il à l’upwelling au large du Pérou ?\",\n"
  "  \"answer\": \"Il faiblit : l’eau qui remonte est plus chaude et plus pauvre en nutriments, la pêche s’effondre.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"marees\",\n"
  "  \"section\": \"marees-vagues\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Marées\",\n"
  "  \"body\": \"La marée vient de l’attraction de la Lune et, un peu moins, du Soleil : l’effet de la Lune est environ deux fois plus fort.\\n\\nSur nos côtes, la marée est **semi-diurne** : deux pleines mers par jour, à environ 12 h 25 min d’intervalle, d’où un décalage d’à peu près 50 minutes par jour.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"vives-eaux\",\n"
  "  \"section\": \"marees-vagues\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Vives et mortes eaux\",\n"
  "  \"body\": \"Quand Lune, Soleil et Terre sont alignés, à la nouvelle et à la pleine lune, leurs effets s’additionnent : ce sont les **vives-eaux**, à fort marnage.\\n\\nAux quartiers de lune, ils se contrarient : **mortes-eaux**, marnage faible. Le cycle complet dure environ 15 jours.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"raz-blanchard\",\n"
  "  \"section\": \"marees-vagues\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Raz Blanchard\",\n"
  "  \"body\": \"Entre le cap de la Hague et Aurigny, la marée force l’eau de la Manche à travers un passage étroit.\\n\\nLes courants de marée y comptent parmi les plus forts d’Europe, **jusqu’à environ 5 m/s** en vives-eaux, soit une dizaine de nœuds. Cette énergie en fait un site étudié pour les hydroliennes.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"vagues\",\n"
  "  \"section\": \"marees-vagues\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Vagues et houle\",\n"
  "  \"body\": \"Les vagues naissent du vent. Tant qu’elles sont sous le vent qui les crée, c’est la **mer du vent**, désordonnée.\\n\\nEn quittant la zone de vent, elles se trient par période et deviennent une **houle** régulière qui peut traverser un océan. Les vagues longues voyagent plus vite que les courtes.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"tsunami\",\n"
  "  \"section\": \"marees-vagues\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Tsunami\",\n"
  "  \"body\": \"Un tsunami est une onde très longue, souvent née d’un séisme sous-marin. Sa vitesse dépend de la profondeur : environ **√(g × h)**.\\n\\nPar 4000 m de fond, cela fait environ 200 m/s, soit plus de 700 km/h. Au large, il ne mesure souvent que quelques dizaines de centimètres ; il grandit en arrivant sur la côte.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-vives-eaux\",\n"
  "  \"section\": \"marees-vagues\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Marées\",\n"
  "  \"question\": \"Les vives-eaux surviennent à quelles phases de la Lune ?\",\n"
  "  \"answer\": \"À la nouvelle lune et à la pleine lune, quand Lune, Soleil et Terre sont alignés.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-maree-decalage\",\n"
  "  \"section\": \"marees-vagues\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Marées\",\n"
  "  \"question\": \"Pourquoi la pleine mer arrive-t-elle environ 50 minutes plus tard chaque jour ?\",\n"
  "  \"answer\": \"La marée semi-diurne suit la Lune : deux pleines mers toutes les 24 h 50 min environ, et non 24 h.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-tsunami\",\n"
  "  \"section\": \"marees-vagues\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Tsunami\",\n"
  "  \"question\": \"Environ à quelle vitesse un tsunami traverse-t-il un océan profond de 4000 m ?\",\n"
  "  \"answer\": \"Environ 200 m/s, soit plus de 700 km/h, car sa vitesse vaut √(g × h).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"phytoplancton\",\n"
  "  \"section\": \"vie-marine\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Phytoplancton\",\n"
  "  \"body\": \"Le **phytoplancton** regroupe les algues microscopiques qui dérivent dans la couche éclairée : diatomées, coccolithophores, dinoflagellés, cyanobactéries.\\n\\nIl réalise environ la moitié de la photosynthèse de la planète, alors qu’il ne représente qu’une infime part de la biomasse végétale.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"production\",\n"
  "  \"section\": \"vie-marine\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Production primaire\",\n"
  "  \"body\": \"Le phytoplancton a besoin de lumière, donc de la surface, et de nutriments (nitrate, phosphate, fer), plus abondants en profondeur.\\n\\nLa production est donc forte là où l’eau profonde remonte (upwellings) ou se mélange (efflorescence de printemps), et faible au centre des gyres, bleus et limpides.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"pompe-bio\",\n"
  "  \"section\": \"vie-marine\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Pompe biologique\",\n"
  "  \"body\": \"Une partie de la matière organique produite en surface coule vers le fond : cadavres, pelotes fécales, agrégats appelés **neige marine**.\\n\\nCe flux emporte du carbone vers l’océan profond, où il peut rester des siècles. C’est la pompe biologique, un des rouages du cycle du carbone.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"omz\",\n"
  "  \"section\": \"vie-marine\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Minimum d’oxygène\",\n"
  "  \"body\": \"Sous certaines zones très productives, la matière organique qui coule est dégradée par les bactéries, qui consomment l’oxygène.\\n\\nSi l’eau est peu renouvelée, il se forme une **zone de minimum d’oxygène**, entre quelques centaines et environ 1000 m, comme dans le Pacifique tropical est ou la mer d’Arabie. Ces zones s’étendent.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"calanus\",\n"
  "  \"section\": \"vie-marine\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Calanus finmarchicus\",\n"
  "  \"body\": \"Un copépode de **3 mm environ**, parmi les animaux les plus abondants de l’Atlantique Nord.\\n\\nIl transforme les micro-algues en lipides très énergétiques, qui nourrissent harengs, larves de morue et baleines. L’hiver, les juvéniles descendent à plusieurs centaines de mètres et y hibernent (diapause).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-phyto\",\n"
  "  \"section\": \"vie-marine\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Phytoplancton\",\n"
  "  \"question\": \"Quelle part de la photosynthèse mondiale le phytoplancton réalise-t-il environ ?\",\n"
  "  \"answer\": \"Environ la moitié.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-pompe\",\n"
  "  \"section\": \"vie-marine\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Pompe biologique\",\n"
  "  \"question\": \"Qu’est-ce que la neige marine ?\",\n"
  "  \"answer\": \"Des agrégats de matière organique (débris, pelotes fécales) qui coulent vers le fond et exportent du carbone.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-omz\",\n"
  "  \"section\": \"vie-marine\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Oxygène\",\n"
  "  \"question\": \"Qu’est-ce qui consomme l’oxygène dans une zone de minimum d’oxygène ?\",\n"
  "  \"answer\": \"Les bactéries qui dégradent la matière organique tombée de la surface, dans une eau peu renouvelée.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"ctd\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"CTD\",\n"
  "  \"body\": \"La **CTD** mesure en continu la conductivité, la température et la pression (donc la profondeur) pendant qu’on la descend au bout d’un câble.\\n\\nElle est souvent montée sur une **rosette** de bouteilles Niskin, qu’on ferme à distance à des profondeurs choisies pour rapporter de l’eau à analyser.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"argo\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Flotteurs Argo\",\n"
  "  \"body\": \"Environ **4000 flotteurs Argo** dérivent dans tous les océans. Chacun reste vers 1000 m, descend à 2000 m, puis remonte en mesurant température et salinité, environ tous les 10 jours.\\n\\nEn surface, il transmet son profil par satellite. Le programme, lancé vers 2000, a transformé l’observation de l’océan intérieur.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"satellites-sst\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"SST par satellite\",\n"
  "  \"body\": \"Les satellites mesurent la température de surface de deux façons. L’**infrarouge** est très fin, mais aveugle sous les nuages.\\n\\nLes **micro-ondes** traversent les nuages, avec une résolution plus grossière. Les produits quotidiens combinent les deux, et les bouées servent de référence.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"altimetrie\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Altimétrie\",\n"
  "  \"body\": \"Un altimètre radar mesure la hauteur de la mer depuis l’espace, à quelques centimètres près.\\n\\nLes bosses et les creux de la surface révèlent les tourbillons et les courants géostrophiques. L’altimétrie suit aussi la **hausse du niveau de la mer** depuis 1993.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"adcp\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"ADCP\",\n"
  "  \"body\": \"L’**ADCP** envoie des impulsions sonores et mesure le décalage Doppler de l’écho renvoyé par les particules en suspension.\\n\\nIl en déduit la vitesse du courant sur toute une colonne d’eau, par couches. On le fixe sous la coque d’un navire, sur un mouillage ou au fond.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"unites\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Unités utiles\",\n"
  "  \"body\": \"**Salinité pratique** : sans unité (échelle PSS-78). **Salinité absolue** : en g/kg (TEOS-10).\\n\\n**Sverdrup** : 1 Sv = 10⁶ m³/s, pour les débits des courants. **Nœud** : 1,852 km/h. **Décibar** : environ 1 m de profondeur.\\n\\n**Anomalie** : écart à une moyenne de référence, par exemple en °C pour la température de surface.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"check-ctd\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Mise à l’eau CTD\",\n"
  "  \"items\": [\n"
  "   \"Capteurs rincés, capuchons retirés\",\n"
  "   \"Bouteilles Niskin armées\",\n"
  "   \"Position et heure notées\",\n"
  "   \"Trempage en surface, pompe amorcée\",\n"
  "   \"Descente à vitesse constante\",\n"
  "   \"Arrêt avec marge au-dessus du fond\",\n"
  "   \"Fermeture des bouteilles à la remontée\",\n"
  "   \"Capteur de conductivité rincé\",\n"
  "   \"Fichier sauvegardé et nommé\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"check-salinite\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Échantillon salinité\",\n"
  "  \"items\": [\n"
  "   \"Flacon rincé trois fois\",\n"
  "   \"Rempli jusqu’à l’épaule\",\n"
  "   \"Goulot essuyé, bouchon serré\",\n"
  "   \"Étiquette : station, bouteille, profondeur\",\n"
  "   \"Report dans le cahier de bord\",\n"
  "   \"Stockage à température du labo\",\n"
  "   \"Analyse avec eau de mer standard\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"check-argo\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Déploiement Argo\",\n"
  "  \"items\": [\n"
  "   \"Numéro WMO vérifié\",\n"
  "   \"Flotteur activé, test de transmission\",\n"
  "   \"Vitesse du navire réduite\",\n"
  "   \"Position et heure GPS notées\",\n"
  "   \"Mise à l’eau sous le vent, en douceur\",\n"
  "   \"Photo du déploiement\",\n"
  "   \"Fiche de déploiement envoyée\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-argo\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Programme Argo\",\n"
  "  \"text\": \"Le programme international Argo : flotteurs, données, cartes de la flotte.\",\n"
  "  \"url\": \"https://argo.ucsd.edu\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-copernicus\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Copernicus Marine\",\n"
  "  \"text\": \"Copernicus Marine : produits océaniques européens, observations et modèles.\",\n"
  "  \"url\": \"https://marine.copernicus.eu\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-ctd\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"CTD\",\n"
  "  \"question\": \"Que signifient les trois lettres CTD ?\",\n"
  "  \"answer\": \"Conductivity, Temperature, Depth : conductivité, température et profondeur (mesurée par la pression).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-argo\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Argo\",\n"
  "  \"question\": \"Jusqu’à quelle profondeur descend un flotteur Argo standard, et à quelle fréquence ?\",\n"
  "  \"answer\": \"Jusqu’à 2000 m, environ tous les 10 jours.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-sverdrup\",\n"
  "  \"section\": \"mesurer\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Unités\",\n"
  "  \"question\": \"Combien vaut un sverdrup ?\",\n"
  "  \"answer\": \"Un million de mètres cubes par seconde (10⁶ m³/s).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"chaleur\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Chaleur absorbée\",\n"
  "  \"body\": \"L’océan a absorbé environ **90 %** de l’excès de chaleur accumulé par le système climatique depuis les années 1970, selon le GIEC (AR6, 2021).\\n\\nGrâce à son énorme capacité thermique, il amortit le réchauffement de l’air, mais cette chaleur le dilate et y restera longtemps.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"puits-co2\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Puits de carbone\",\n"
  "  \"body\": \"L’océan absorbe environ **un quart** du CO₂ émis chaque année par les activités humaines.\\n\\nLe CO₂ se dissout en surface, puis les pompes physique (plongée d’eau froide) et biologique l’entraînent en profondeur. Ce service a un coût : l’acidification.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"acidification\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Acidification\",\n"
  "  \"body\": \"En se dissolvant, le CO₂ forme de l’acide carbonique. Le pH moyen de surface est passé d’environ **8,2 à 8,1** depuis l’époque préindustrielle.\\n\\nSur une échelle logarithmique, cela représente environ 30 % d’ions hydrogène (H+) en plus. Coraux, coquillages et certains planctons calcaires peinent davantage à construire leur squelette.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"niveau-mer\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Niveau de la mer\",\n"
  "  \"body\": \"Le niveau moyen de la mer a monté d’environ **20 cm entre 1901 et 2018** (GIEC, AR6), et la hausse accélère : environ 3,7 mm/an entre 2006 et 2018.\\n\\nDeux causes : la dilatation de l’eau qui se réchauffe et la fonte des glaciers et des calottes. La hausse n’est pas uniforme : elle varie d’une région à l’autre.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-chaleur\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Chaleur\",\n"
  "  \"question\": \"Quelle part de l’excès de chaleur dû au réchauffement l’océan a-t-il absorbé depuis les années 1970 ?\",\n"
  "  \"answer\": \"Environ 90 %, selon le GIEC (AR6).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-ph\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Acidification\",\n"
  "  \"question\": \"De combien le pH de surface de l’océan a-t-il baissé depuis l’époque préindustrielle ?\",\n"
  "  \"answer\": \"D’environ 0,1 unité (de 8,2 à 8,1), soit environ 30 % d’ions hydrogène (H+) en plus.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-niveau\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Niveau de la mer\",\n"
  "  \"question\": \"Quelles sont les deux grandes causes de la montée du niveau de la mer ?\",\n"
  "  \"answer\": \"La dilatation thermique de l’eau et la fonte des glaciers et des calottes.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-invisibleocean\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"!nvisibleOcean\",\n"
  "  \"text\": \"Voir tout cela sur un globe 3D : température, salinité, courants, Argo, coupes, 14 leçons.\",\n"
  "  \"url\": \"https://invisibleocean.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"climat\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Atlantid!a Lite\",\n"
  "  \"body\": \"Écrit par Julien et Claude, l’IA\"\n"
  " }\n"
  "]\n";

// generated from editions/<slug>/*.json: Vulcan!a Lite, Yog!a Lite, C@tlas (from the Editions chat, v1.0.0)
const char VU_EDITION[] =
  "{\n"
  " \"name\": \"Vulcan!a Lite\",\n"
  " \"slug\": \"vulcania-lite\",\n"
  " \"version\": \"1.0.1\",\n"
  " \"lang\": \"en\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"sources\": [\n"
  "  \"Global Volcanism Program, Smithsonian Institution. Volcanoes of the World, v. 5.4.0 (7 Aug 2026). https://volcano.si.edu\",\n"
  "  \"Global Volcanism Program. FAQ: How many active volcanoes are there? Statistics at database update of 31 Mar 2026. https://volcano.si.edu\",\n"
  "  \"Smithsonian / USGS Weekly Volcanic Activity Report. https://volcano.si.edu\",\n"
  "  \"USGS Volcano Hazards Program: hazards, volcano alert levels and aviation color codes. https://www.usgs.gov/programs/VHP\",\n"
  "  \"USGS FAQ: How dangerous are pyroclastic flows? Table of deadliest eruptions since 1500 AD. https://www.usgs.gov\",\n"
  "  \"USGS Volcano Hazards Program glossary: Plinian eruption, Vesuvius 79 AD. https://www.usgs.gov\",\n"
  "  \"USGS Fact Sheet 027-00 (2000). Volcanic Ash Fall: A Hard Rain of Abrasive Particles. https://pubs.usgs.gov/fs/fs027-00/\",\n"
  "  \"USGS Volcanic Ash Impacts and Mitigation: roof loading, Pinatubo 1991. https://volcanoes.usgs.gov/volcanic_ash/\",\n"
  "  \"USGS Fact Sheet 113-97 (1997). The Cataclysmic 1991 Eruption of Mount Pinatubo, Philippines.\",\n"
  "  \"Newhall, C. G. and Self, S. (1982). The volcanic explosivity index (VEI). Journal of Geophysical Research 87 (C2), 1231-1238.\",\n"
  "  \"Kling, G. W. et al. (1987). The 1986 Lake Nyos gas disaster in Cameroon, West Africa. Science 236, 169-175.\",\n"
  "  \"Encyclopaedia Britannica: Tambora eruption of 1815 (erupted volume). https://www.britannica.com\",\n"
  "  \"National Geographic Education: The hazards of pyroclastic flows (speeds and temperatures).\",\n"
  "  \"NASA FIRMS, Fire Information for Resource Management System. https://firms.modaps.eosdis.nasa.gov\",\n"
  "  \"Japan Meteorological Agency: Volcanic Alert Levels. PVMBG / MAGMA Indonesia: volcanic activity levels.\",\n"
  "  \"GeoNet (GNS Science, New Zealand): Whakaari / White Island eruption of 9 December 2019.\",\n"
  "  \"VolcanoBro, InvisibleChaos: how the globe derives its colors. https://volcanobro.netlify.app\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"inside\",\n"
  "   \"title\": \"How volcanoes work\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"eruptions\",\n"
  "   \"title\": \"Eruptions\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"hazards\",\n"
  "   \"title\": \"Hazards\",\n"
  "   \"order\": 3\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"monitoring\",\n"
  "   \"title\": \"Watching volcanoes\",\n"
  "   \"order\": 4\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"living-nearby\",\n"
  "   \"title\": \"Living nearby\",\n"
  "   \"order\": 5\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"data\",\n"
  "   \"title\": \"The data\",\n"
  "   \"order\": 6\n"
  "  }\n"
  " ]\n"
  "}\n";
const char VU_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"what-is-a-volcano\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"What is a volcano\",\n"
  "  \"body\": \"A volcano is an opening in the crust where molten rock, gas and fragments reach the surface. The word also names the mountain that eruptions build around it.\\n\\nUnderground the molten rock is **magma**; once it erupts it is **lava**.\\n\\nThe Smithsonian counts about **1,215 volcanoes** thought to have erupted in the last 12,000 years (database update, March 2026).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"magma\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Magma\",\n"
  "  \"body\": \"Magma is molten rock carrying dissolved gas: mostly water vapor, with CO₂ and SO₂.\\n\\nWhat matters most is how runny it is. **Basalt** is fluid and hot (about 1,000 to 1,200 °C): gas escapes easily and lava flows. **Rhyolite** is sticky and cooler (about 650 to 800 °C): gas stays trapped, pressure builds, and the magma can shatter explosively.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"where-they-form\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Where they form\",\n"
  "  \"body\": \"Most volcanoes grow in three settings.\\n\\n**Subduction zones**: one plate dives under another and water from it helps rock melt. This builds the Pacific Ring of Fire.\\n\\n**Spreading ridges**: plates pull apart, mostly on the sea floor. Iceland is a ridge raised above the sea by a hotspot.\\n\\n**Hotspots**: a plume of hot mantle rises under a plate, as under Hawaii.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"volcano-shapes\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Volcano shapes\",\n"
  "  \"body\": \"**Shield**: broad, gentle slopes built by runny lava. Mauna Loa, Hawaii.\\n\\n**Stratovolcano**: steep cone of alternating lava and ash, often explosive. Fuji, Japan.\\n\\n**Cinder cone**: small, built from lava fragments in one eruption.\\n\\n**Caldera**: a basin left when the ground collapses into an emptied magma chamber.\\n\\n**Lava dome**: a pile of sticky lava too stiff to flow far.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"ring-of-fire\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Ring of Fire\",\n"
  "  \"body\": \"The Pacific Ring of Fire is a chain of subduction zones around the Pacific Ocean: from New Zealand through Indonesia, the Philippines, Japan and Kamchatka, across Alaska and down the Americas to Chile.\\n\\nThe Smithsonian counts **687 Holocene volcanoes** there, about **57%** of the total, in 41 volcanic regions.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-explosive\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Runny or sticky?\",\n"
  "  \"question\": \"Which magma is more likely to erupt explosively: runny basalt or sticky rhyolite?\",\n"
  "  \"answer\": \"Sticky rhyolite. Its gas cannot escape easily, so pressure builds until the magma shatters.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-shield\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Gentle giant\",\n"
  "  \"question\": \"Mauna Loa has broad, gentle slopes built by runny lava. What type of volcano is it?\",\n"
  "  \"answer\": \"A **shield volcano**.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-holocene\",\n"
  "  \"section\": \"inside\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Why 'Holocene'?\",\n"
  "  \"question\": \"Why does the Smithsonian prefer the term 'Holocene volcano' to 'active volcano'?\",\n"
  "  \"answer\": \"Quiet spells can last thousands of years, so human memory is a poor guide. 'Holocene' simply means eruptions within roughly the last 12,000 years.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"two-ways-out\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Two ways out\",\n"
  "  \"body\": \"Eruptions come in two broad kinds.\\n\\n**Effusive**: lava pours out and flows away. Common with runny basalt, as in Hawaii and Iceland. Destructive to property, rarely deadly.\\n\\n**Explosive**: gas-rich magma shatters into ash and pumice, thrown high into the sky. Common with sticky magma at stratovolcanoes. Most volcano deaths come from explosive eruptions and what follows them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"eruption-styles\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Eruption styles\",\n"
  "  \"body\": \"Volcanologists name styles after famous examples, from gentle to violent:\\n\\n**Hawaiian**: lava fountains and flows.\\n**Strombolian**: short bursts that throw glowing lumps.\\n**Vulcanian**: sharp blasts of ash and blocks.\\n**Plinian**: a huge column of ash and gas rising into the stratosphere, above about 11 km, followed by ash fall and pyroclastic flows.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"vei\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The VEI scale\",\n"
  "  \"body\": \"The **Volcanic Explosivity Index** (Newhall and Self, 1982) rates eruptions from 0 to 8, mostly by volume of tephra and column height.\\n\\nIt is logarithmic: above VEI 2, each step means about **ten times** more material.\\n\\nVEI 4: at least 0.1 km³\\nVEI 6: at least 10 km³ (Pinatubo 1991)\\nVEI 7: at least 100 km³ (Tambora 1815)\\nVEI 8: at least 1,000 km³\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"vesuvius-79\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Vesuvius, 79 CE\",\n"
  "  \"body\": \"In 79 CE, Vesuvius buried the Roman towns of Pompeii and Herculaneum under ash, pumice and pyroclastic flows. About 2,000 people died there, according to the USGS.\\n\\nPliny the Younger watched from across the bay and wrote the first detailed eyewitness account of an eruption. His uncle, Pliny the Elder, died leading a rescue. Plinian eruptions are named after the nephew.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"tambora-1815\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Tambora, 1815\",\n"
  "  \"body\": \"Tambora, on Sumbawa in Indonesia, produced the largest eruption in recorded history in April 1815: VEI 7, about 50 km³ of magma.\\n\\nThe USGS puts the death toll at about **92,000**, mostly from starvation after ash ruined crops.\\n\\nSulfur gas in the stratosphere cooled the planet: 1816 became the 'year without a summer' in Europe and North America.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"pinatubo-1991\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Pinatubo, 1991\",\n"
  "  \"body\": \"Mount Pinatubo in the Philippines erupted on 15 June 1991: VEI 6, the second largest eruption of the 20th century.\\n\\nScientists spotted the unrest early and tens of thousands were evacuated. The USGS credits the forecast with saving **at least 5,000 lives**.\\n\\nIts sulfur haze cooled global temperatures by about 0.5 °C for a year or more.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-vei-step\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"One step up\",\n"
  "  \"question\": \"Above VEI 2, how much more erupted material does each step on the VEI scale mean?\",\n"
  "  \"answer\": \"About **ten times** more. The scale is logarithmic.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-deadliest\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Deadliest\",\n"
  "  \"question\": \"Which eruption is the deadliest on record, and what killed most of its victims?\",\n"
  "  \"answer\": \"**Tambora, 1815**: about 92,000 deaths (USGS), mostly from starvation after ash destroyed crops.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-increasing\",\n"
  "  \"section\": \"eruptions\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"More than ever?\",\n"
  "  \"question\": \"Has volcanic activity increased in recent decades?\",\n"
  "  \"answer\": \"No, according to the Smithsonian GVP. More eruptions are recorded now because satellites and better communication catch small ones that went unnoticed before.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"what-kills\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"What kills\",\n"
  "  \"body\": \"Lava rarely kills: it usually moves slowly enough to walk away from.\\n\\nThe USGS table of deadliest eruptions since 1500 names other killers: starvation (Tambora 1815), tsunami (Krakatau 1883, about 36,000), pyroclastic flows (Mont Pelée 1902, about 30,000) and mudflows (Nevado del Ruiz 1985, about 25,000).\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"pyroclastic-flows\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Pyroclastic flows\",\n"
  "  \"body\": \"A pyroclastic flow is an avalanche of hot gas, ash and rock that races down a volcano's slopes, usually faster than **100 km/h** and at about 200 to 700 °C.\\n\\nIt cannot be outrun. The only protection is to be out of its path before it forms.\\n\\nIn 1902, one destroyed the town of Saint-Pierre on Martinique in minutes.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"lahars\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Lahars\",\n"
  "  \"body\": \"A lahar is a volcanic mudflow: water mixed with ash and rock, flowing like wet concrete down river valleys, sometimes tens of kilometres.\\n\\nMelted snow and ice, crater lakes or heavy rain on fresh ash can all trigger one, even years after an eruption.\\n\\nIn 1985 a lahar from Nevado del Ruiz buried Armero, Colombia: about 23,000 to 25,000 dead.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"tephra-and-ash\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Tephra and ash\",\n"
  "  \"body\": \"**Tephra** is everything a volcano throws into the air, from blocks to dust. The finest part is **ash**.\\n\\nVolcanic ash is not soot. It is tiny shards of rock and glass: hard, abrasive, it does not dissolve in water, and it conducts electricity when wet.\\n\\nIt harms lungs and eyes, jams engines and machinery, and cuts power.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"ash-on-roofs\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Ash on roofs\",\n"
  "  \"body\": \"Ash is heavy, and rain can make it up to **twice as heavy**.\\n\\nThe USGS notes that roof collapse usually needs more than 100 mm of ash, but flat, long-span or poorly built roofs can fail with less.\\n\\nDuring Pinatubo in 1991, a typhoon soaked the falling ash, and roof collapses killed nearly 300 people.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"volcanic-gases\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Volcanic gases\",\n"
  "  \"body\": \"Volcanoes release water vapor, CO₂, SO₂ and hydrogen sulfide.\\n\\n**CO₂** is invisible and heavier than air: it can pool in hollows and suffocate. In 1986, a burst of CO₂ from Lake Nyos, a volcanic lake in Cameroon, killed about 1,700 people.\\n\\n**SO₂** irritates eyes and lungs and forms volcanic smog ('vog') downwind.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"collapse-tsunami\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Collapse and tsunami\",\n"
  "  \"body\": \"Volcanoes can trigger tsunamis when they explode at sea, when flows enter the water, or when a flank collapses.\\n\\nKrakatau in 1883 caused tsunamis that killed about 36,000 people. At Unzen, Japan, in 1792, a collapse and tsunami killed about 15,000 (USGS).\\n\\nCoasts near island volcanoes need tsunami warnings as well as eruption alerts.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-armero\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Armero, 1985\",\n"
  "  \"question\": \"What killed most of the victims at Armero, Colombia, in 1985?\",\n"
  "  \"answer\": \"A **lahar**, a volcanic mudflow from Nevado del Ruiz, fed by melted summit ice.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-outrun\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Run for it?\",\n"
  "  \"question\": \"Can a person outrun a pyroclastic flow?\",\n"
  "  \"answer\": \"No. They usually move faster than 100 km/h. The only protection is to leave the danger zone before one forms.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-ash\",\n"
  "  \"section\": \"hazards\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Not soot\",\n"
  "  \"question\": \"Why does volcanic ash damage lungs and machines so badly?\",\n"
  "  \"answer\": \"It is not soot but tiny shards of rock and glass: hard, sharp and abrasive. It does not dissolve in water.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"earthquakes\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Earthquakes\",\n"
  "  \"body\": \"Rising magma cracks the rock around it, causing swarms of small earthquakes. Moving fluids add long-period events and a steady hum called **tremor**.\\n\\nSeismometers around a volcano track where quakes happen and whether they grow shallower.\\n\\nA swarm is a warning sign, not a countdown: many swarms end without an eruption.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"ground-swelling\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Ground swelling\",\n"
  "  \"body\": \"When magma collects underground, the surface above can swell by centimetres or more.\\n\\nTiltmeters and GPS stations measure this on the ground.\\n\\n**InSAR** does it from space: radar satellites image the same area at different times, and comparing the images maps ground movement across a whole volcano, even where no instrument stands.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"gas-signals\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Gas signals\",\n"
  "  \"body\": \"Gas escapes from magma before it erupts. Observatories measure how much SO₂ and CO₂ a volcano releases, and in what ratio.\\n\\nA jump in output, or a change in the ratio, can mean fresh magma is rising.\\n\\nMeasurements come from ground stations, walkers with portable instruments, drones and satellites.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"heat-from-space\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Heat from space\",\n"
  "  \"body\": \"Satellite sensors such as MODIS and VIIRS pick up hot spots on the ground: lava flows, glowing domes, crater lakes heating up.\\n\\nNASA's FIRMS system delivers these detections worldwide, usually within about three hours.\\n\\nThe limits: clouds hide the ground, and a sensor cannot tell lava from a fire. Heat is a clue, not a diagnosis.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"alert-levels\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Alert levels\",\n"
  "  \"body\": \"Observatories publish official alert levels. The USGS uses four, from lowest to highest: **Normal, Advisory, Watch, Warning**.\\n\\nOther countries use their own scales: Japan has five levels, Indonesia four.\\n\\nThe number means different things in different places, so read it on the local observatory's terms.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"aviation-colors\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Aviation colors\",\n"
  "  \"body\": \"A separate code protects aircraft: **Green, Yellow, Orange, Red**, rating the risk of ash in the air.\\n\\nAsh melts inside jet engines and can make them fail. Nine Volcanic Ash Advisory Centers around the world track ash clouds for pilots.\\n\\nIn 2010, ash from Eyjafjallajökull in Iceland closed much of Europe's airspace for days.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"forecast\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Forecast, not predict\",\n"
  "  \"body\": \"Nobody can give the date of an eruption. Scientists forecast: they combine quakes, swelling, gas and heat into probabilities.\\n\\nUnrest often comes before an eruption, but not always, and many episodes of unrest fade away.\\n\\nIt works when warnings reach people in time: at Pinatubo in 1991, evacuations saved thousands of lives.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-usgs-levels\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Four levels\",\n"
  "  \"question\": \"What are the four USGS volcano alert levels, from lowest to highest?\",\n"
  "  \"answer\": \"**Normal, Advisory, Watch, Warning.**\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-insar\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"InSAR\",\n"
  "  \"question\": \"What does InSAR measure at volcanoes, and how?\",\n"
  "  \"answer\": \"Ground movement, by comparing radar images of the same area taken by satellite at different times.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-aviation\",\n"
  "  \"section\": \"monitoring\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Ash and aircraft\",\n"
  "  \"question\": \"Why do aviation authorities close airspace for volcanic ash?\",\n"
  "  \"answer\": \"Ash can melt inside jet engines and make them fail. It also scours windows and blocks sensors.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"before\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Before it happens\",\n"
  "  \"body\": \"Around many volcanoes, authorities publish **hazard maps**: where lava, flows, lahars and ash are most likely to go.\\n\\nKnow your zone, your evacuation route and how alerts reach you: radio, phone alerts, sirens.\\n\\nPrepare an ash kit and a plan for pets and people who need help to move.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"evacuation\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"When told to leave\",\n"
  "  \"body\": \"Leave early. Evacuations work best before an eruption peaks.\\n\\nFollow the official routes, not shortcuts, and keep away from river valleys: lahars and pyroclastic flows follow them.\\n\\nDo not go back until authorities say it is safe. Lahars can come hours or years after an eruption.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"visiting\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Visiting a volcano\",\n"
  "  \"body\": \"Active volcanoes attract many visitors, and conditions can change within minutes, often with no visible warning.\\n\\nOn 9 December 2019, an eruption at Whakaari / White Island, New Zealand, killed 22 people, most of them on a guided tour of the crater.\\n\\nRespect exclusion zones and closures, even when the volcano looks quiet.\\n\\nLocal authorities decide: follow them.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"ash-kit\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Ash kit\",\n"
  "  \"items\": [\n"
  "   \"N95 / FFP2 masks for everyone\",\n"
  "   \"Goggles (not contact lenses)\",\n"
  "   \"Bottled water for several days\",\n"
  "   \"Food for several days\",\n"
  "   \"Torch and spare batteries\",\n"
  "   \"Battery or wind-up radio\",\n"
  "   \"Plastic sheeting and tape\",\n"
  "   \"First aid kit\",\n"
  "   \"Regular medicines\",\n"
  "   \"Local authorities decide: follow them.\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"during-ashfall\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"During ashfall\",\n"
  "  \"items\": [\n"
  "   \"Stay indoors if you can\",\n"
  "   \"Close windows, doors and vents\",\n"
  "   \"Switch off air intake and fans\",\n"
  "   \"Wear a mask and goggles outside\",\n"
  "   \"Avoid driving: ash is slippery\",\n"
  "   \"Cover electronics and water tanks\",\n"
  "   \"Keep gutters clear only if safe\",\n"
  "   \"Local authorities decide: follow them.\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"before-a-hike\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Before a volcano hike\",\n"
  "  \"items\": [\n"
  "   \"Check today's official alert level\",\n"
  "   \"Check closures and exclusion zones\",\n"
  "   \"Go with a licensed guide if required\",\n"
  "   \"Tell someone your route and return time\",\n"
  "   \"Carry water, headlamp and a mask\",\n"
  "   \"Turn back if gas smell grows strong\",\n"
  "   \"Local authorities decide: follow them.\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-valleys\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Which way?\",\n"
  "  \"question\": \"During an evacuation, why should you keep away from river valleys below a volcano?\",\n"
  "  \"answer\": \"Lahars and pyroclastic flows follow valleys. Higher ground away from channels is safer.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wet-ash\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Wet ash\",\n"
  "  \"question\": \"Why is ash more dangerous on a roof after rain?\",\n"
  "  \"answer\": \"Water can make ash up to twice as heavy, and weak or flat roofs can collapse under the load.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-hike-check\",\n"
  "  \"section\": \"living-nearby\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Before you go\",\n"
  "  \"question\": \"What should you check on the day you hike an active volcano?\",\n"
  "  \"answer\": \"The official alert level and any closures, from the observatory or local authorities.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"smithsonian-gvp\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Smithsonian GVP\",\n"
  "  \"body\": \"The **Global Volcanism Program** at the Smithsonian Institution keeps the reference list of the world's volcanoes: Volcanoes of the World.\\n\\nIt covers every volcano thought to have erupted in the last 12,000 years, about 1,215 today, each with a number, location, type and dated eruptions.\\n\\nVolcanoBro's atlas of 1,214 volcanoes comes from this database.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"weekly-report\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The weekly report\",\n"
  "  \"body\": \"Every Wednesday, the Smithsonian and the USGS publish the **Weekly Volcanic Activity Report**: a summary of new and ongoing activity worldwide, drawn from observatories, aviation ash centers and news.\\n\\nIt is a quick first report, not the final word: details can be revised later.\\n\\nVolcanoBro reads it to decide which volcanoes count as 'reported'.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"nasa-firms\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"NASA FIRMS\",\n"
  "  \"body\": \"**FIRMS** (Fire Information for Resource Management System) is a NASA service that maps hot spots seen by satellites, worldwide, usually within about three hours.\\n\\nIt was built for wildfires, so it cannot tell lava from a fire, and clouds hide what lies beneath them.\\n\\nVolcanoBro uses it as a second, independent signal: heat seen from space.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"volcanobro-colors\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"VolcanoBro colors\",\n"
  "  \"body\": \"VolcanoBro colors each volcano from two sources:\\n\\n**Red, Erupting**: in the weekly report and hot from space.\\n**Orange, Reported**: in the report, no heat seen now.\\n**Yellow, Mystery heat**: heat seen, but not in the report.\\n**Green, Quiet**: neither.\\n\\nThe colors are worked out again every time the globe opens. Nothing is set by hand.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"not-an-alert\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Not an alert level\",\n"
  "  \"body\": \"VolcanoBro's colors look like the aviation color code, but they are **not official alerts**. No observatory issues them: the app derives them from a weekly report and satellite heat.\\n\\nA red volcano in VolcanoBro is not a warning to act on, and a green one is not a promise of safety.\\n\\nFor decisions, use the local observatory and local authorities.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"names-differ\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Names differ\",\n"
  "  \"body\": \"The same volcano can go by several names. The Smithsonian lists Sakurajima under **Aira**, the caldera it grew in. Anak Krakatau is the young cone inside the old Krakatau.\\n\\nTransliteration adds more: Klyuchevskoy is also written Kliuchevskoi.\\n\\nWhen sources seem to disagree, compare locations, not just names.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-yellow\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Yellow on the globe\",\n"
  "  \"question\": \"In VolcanoBro, what does a yellow volcano mean?\",\n"
  "  \"answer\": \"Satellites see heat there, but the weekly report does not mention it: possibly new activity, possibly a fire nearby.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-red-official\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Red means official?\",\n"
  "  \"question\": \"Is a red volcano in VolcanoBro an official warning?\",\n"
  "  \"answer\": \"No. VolcanoBro derives its colors from a report and satellite heat. Official alert levels come from observatories and local authorities.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-firms-fire\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Lava or fire?\",\n"
  "  \"question\": \"Why can satellite heat data mistake a forest fire for volcanic activity?\",\n"
  "  \"answer\": \"The sensor detects heat, not its source. From orbit, a fire on a volcano's slopes can look the same as lava.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-volcanobro\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"VolcanoBro\",\n"
  "  \"text\": \"The live globe behind this edition: every Holocene volcano, colored by this week's reports and satellite heat.\",\n"
  "  \"url\": \"https://volcanobro.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-gvp\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Smithsonian GVP\",\n"
  "  \"text\": \"The Global Volcanism Program: the volcano database, the weekly activity report and eruption histories.\",\n"
  "  \"url\": \"https://volcano.si.edu\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"qr-usgs\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"USGS Volcano Hazards\",\n"
  "  \"text\": \"The USGS Volcano Hazards Program: US observatories, alert levels and plain-language hazard guides.\",\n"
  "  \"url\": \"https://www.usgs.gov/programs/VHP\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"data\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Vulcan!a Lite\",\n"
  "  \"body\": \"Written by Julien and Claude the AI\"\n"
  " }\n"
  "]\n";
const char YO_EDITION[] =
  "{\n"
  " \"name\": \"Yog!a Lite\",\n"
  " \"slug\": \"yogia-lite\",\n"
  " \"version\": \"1.0.1\",\n"
  " \"lang\": \"en\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"sources\": [\n"
  "  \"National Center for Complementary and Integrative Health (NCCIH). \\\"Yoga: Effectiveness and Safety.\\\" nccih.nih.gov, reviewed 2025.\",\n"
  "  \"NCCIH Clinical Digest. \\\"Yoga for Health.\\\" February 2020.\",\n"
  "  \"Clarke TC, Barnes PM, Black LI, Stussman BJ, Nahin RL. \\\"Use of Yoga, Meditation, and Chiropractors Among U.S. Adults Aged 18 and Over.\\\" NCHS Data Brief no. 325, 2018.\",\n"
  "  \"Balban MY, Neri E, Kogon MM, Weed L, Nouriani B, Jo B, Holl G, Zeitzer JM, Spiegel D, Huberman AD. \\\"Brief Structured Respiration Practices Enhance Mood and Reduce Physiological Arousal.\\\" Cell Reports Medicine, 2023.\",\n"
  "  \"Bae D, Matthews JJL, Chen JJ, Mah L. \\\"Increased Exhalation to Inhalation Ratio During Breathing Enhances High-Frequency Heart Rate Variability in Healthy Adults.\\\" Psychophysiology, 2021.\",\n"
  "  \"Lieberman MD, Eisenberger NI, Crockett MJ, Tom SM, Pfeifer JH, Way BM. \\\"Putting Feelings Into Words: Affect Labeling Disrupts Amygdala Activity in Response to Affective Stimuli.\\\" Psychological Science, 2007.\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"breath\",\n"
  "   \"title\": \"Breath\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"postures\",\n"
  "   \"title\": \"Postures\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"anger\",\n"
  "   \"title\": \"Anger & Nervous System\",\n"
  "   \"order\": 3\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"practice\",\n"
  "   \"title\": \"Practice\",\n"
  "   \"order\": 4\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"roots\",\n"
  "   \"title\": \"Roots\",\n"
  "   \"order\": 5\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"teaching\",\n"
  "   \"title\": \"Teaching\",\n"
  "   \"order\": 6\n"
  "  }\n"
  " ]\n"
  "}\n";
const char YO_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"breath-diaphragm\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Belly breathing\",\n"
  "  \"body\": \"Most adults under stress breathe high and shallow, into the chest. Diaphragmatic breathing moves the belly instead: on the inhale it rises, on the exhale it falls. This uses the full lungs, slows the breathing rate, and is the base every other breath technique in this edition builds on.\\nPractice: one hand on the chest, one on the belly. Breathe so only the lower hand moves. Five minutes is enough to feel the shift.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breath-exhale-ratio\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Longer exhale\",\n"
  "  \"body\": \"A 2021 study found that stretching the exhale to about twice the inhale (2:1) measurably increased heart rate variability, a marker of parasympathetic activity, compared with equal inhale and exhale. The effect lasted several minutes after the breathing stopped.\\nA simple ratio to try: inhale for a count of 4, exhale for a count of 8. No need to force it, just lean the count that direction.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breath-physio-sigh\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The physiological sigh\",\n"
  "  \"body\": \"Two inhales through the nose, the second short and on top of the first, then one long exhale through the mouth. A 2023 Stanford trial found that five minutes of this pattern, done daily for a month, reduced physiological arousal and improved mood more than an equal amount of mindfulness meditation.\\nUse it any time tension spikes, mid-class or off the mat. One to three cycles is enough for a noticeable reset.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breath-ujjayi\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Ujjayi (ocean breath)\",\n"
  "  \"body\": \"A soft contraction at the back of the throat turns ordinary breathing into an audible, even hiss on both the inhale and exhale, like the sound of the ocean or of Darth Vader. Traditionally it is used to keep the mind anchored to the breath during a physical practice.\\nTry it by exhaling with your mouth open as if fogging a mirror, then close the mouth and keep that same throat shape while breathing through the nose.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breath-kapalabhati\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Kapalabhati, with care\",\n"
  "  \"body\": \"\\\"Skull-shining breath\\\": short, forceful exhales through the nose, powered by the belly, with passive inhales between. Traditionally used to energise and clear the mind before a session.\\nIt raises heart rate and can cause light-headedness. Skip it if pregnant, or with high blood pressure, glaucoma, or a hernia. Stop immediately if dizzy. This is a stimulating technique, not a calming one, so it does not belong in the release phase of Invisible Chaos Yoga.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breath-c-safety\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Before you retain\",\n"
  "  \"items\": [\n"
  "   \"Never breath-hold on a full stomach\",\n"
  "   \"Stop at the first sign of dizziness\",\n"
  "   \"Skip forceful breath work if pregnant\",\n"
  "   \"Skip it too with high blood pressure\",\n"
  "   \"Or glaucoma, or a recent hernia\",\n"
  "   \"Breathe through the nose unless cued\",\n"
  "   \"A pushed breath is a wrong breath\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"breath-p-ratio\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Exhale ratio\",\n"
  "  \"question\": \"In the 2021 study on exhale ratio, roughly how much longer was the exhale than the inhale?\",\n"
  "  \"answer\": \"About twice as long (a 2:1 ratio), for example a 4-count inhale and an 8-count exhale.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breath-p-sigh\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Sigh structure\",\n"
  "  \"question\": \"What are the three parts of a physiological sigh, in order?\",\n"
  "  \"answer\": \"A full inhale through the nose, a second short top-up inhale, then one long exhale through the mouth.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"breath-p-kapalabhati\",\n"
  "  \"section\": \"breath\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Kapalabhati direction\",\n"
  "  \"question\": \"In kapalabhati, is the effort on the inhale or the exhale?\",\n"
  "  \"answer\": \"The exhale. It is short and forceful, pushed from the belly; the inhale that follows is passive.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"post-families\",\n"
  "  \"section\": \"postures\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Families of asana\",\n"
  "  \"body\": \"Most poses fall into a handful of families: standing (Warrior, Triangle), seated (Easy Pose, forward folds), backbends (Camel, Wheel), twists, balances (Tree), and inversions (Legs-Up-The-Wall, Shoulderstand). A class usually samples several families rather than staying in one.\\nKnowing the family a pose belongs to helps predict what it will ask of the body, even the first time you meet it.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"post-gravity\",\n"
  "  \"section\": \"postures\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Gravity work\",\n"
  "  \"body\": \"Invisible Chaos Yoga's own term for a specific way of stretching: instead of muscling into a shape, you find a position where the weight of the body itself does the work, and you simply stop resisting it.\\nA seated forward fold is a clear example: let the head and ribcage hang rather than reaching the hands toward the feet. The stretch arrives on its own, on its own schedule, without effort. This is the soft, easy default this practice returns to between the sound-release work.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"post-alignment\",\n"
  "  \"section\": \"postures\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"General alignment cues\",\n"
  "  \"body\": \"A short list that applies across most standing poses: knees track over the toes, not inward. The spine stays long rather than collapsed. Shoulders stay away from the ears. The core stays gently engaged rather than braced hard.\\nCues are starting points, not commands: every body is built differently, and a cue that opens one student's hips may do nothing for another's.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"post-mistakes\",\n"
  "  \"section\": \"postures\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Common mistakes\",\n"
  "  \"body\": \"Locking the knees or elbows straight, which puts strain on the joint instead of the muscle. Holding the breath during effort, which spikes tension instead of releasing it. Forcing range of motion to match a neighbour's, which ignores what today's body actually has available.\\nThe fix for all three is the same: soften the joint slightly, breathe through the shape, and let range come from repetition over weeks, not from force in one class.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"post-props\",\n"
  "  \"section\": \"postures\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Props & mods\",\n"
  "  \"body\": \"A block brings the floor closer, useful under the hand in a triangle pose or under the seat in a seated fold. A strap extends the reach in poses like a seated forward fold when the hands cannot reach the feet. A wall offers balance support in standing poses.\\nProps are not a lesser version of a pose, they are the same pose with the geometry adjusted to fit the body actually in the room.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"post-p-family\",\n"
  "  \"section\": \"postures\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Pose family\",\n"
  "  \"question\": \"Which family does Downward-Facing Dog belong to?\",\n"
  "  \"answer\": \"It is usually classed with the inversions, since the hips rise above the heart, though some schools group it as a standing pose.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"post-p-block\",\n"
  "  \"section\": \"postures\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"What a block does\",\n"
  "  \"question\": \"In a standing forward fold, what problem does placing hands on blocks solve?\",\n"
  "  \"answer\": \"It brings the floor closer, so the spine can stay longer instead of rounding to reach the actual floor.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"post-p-pain\",\n"
  "  \"section\": \"postures\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Reading pain\",\n"
  "  \"question\": \"What is the difference between the stretch sensation a pose is meant to give, and a signal to back off?\",\n"
  "  \"answer\": \"A stretch feels like tension that eases with breath. A sharp, pinching, or joint-level pain does not ease, and means back off now.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-fight-flight\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Fight-or-flight\",\n"
  "  \"body\": \"Under a perceived threat, the sympathetic nervous system fires: heart rate rises, breathing gets shallow and fast, muscles tense. This is an old, fast survival system, and it does not distinguish well between a real danger and a frustrating email.\\nThe parasympathetic system is the counterweight: it is what breathing practices, and much of a soft yoga practice, are designed to activate on purpose.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-hrv\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"What HRV measures\",\n"
  "  \"body\": \"Heart rate variability (HRV) is not your heart rate, it is how much the gap between heartbeats changes moment to moment. A healthy, flexible nervous system produces more variability, not less. HRV rises during the exhale and falls during the inhale, driven by the vagus nerve.\\nHigher HRV at rest is generally read as a sign of stronger vagal tone and better stress recovery capacity, not of a perfectly steady heartbeat.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-sigh-evidence\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The sigh study\",\n"
  "  \"body\": \"The 2023 Stanford trial (Balban et al.) put 114 people through one of four daily five-minute practices for a month: cyclic sighing, cyclic hyperventilation, box breathing, or mindfulness meditation. Cyclic sighing produced the largest gains in mood and the largest drop in resting breathing rate of the four.\\nThis is one study, not a settled fact for all populations, but it is the strongest direct evidence in this edition for a specific breath pattern over general meditation.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-labeling\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Naming what you feel\",\n"
  "  \"body\": \"In a 2007 UCLA brain-imaging study, people who put a feeling into words while looking at a distressing image showed reduced activity in the amygdala, the brain's threat-alarm region, compared with people who did not label it. Simply naming the emotion, silently or aloud, appeared to turn the alarm down.\\nThis gives a physiological reason for a simple habit: say what you feel, out loud or in your head, before trying to fix it.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-vocal-honesty\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Sound release, honest\",\n"
  "  \"body\": \"Invisible Chaos Yoga's grunts and sighs during breathwork sit closer to tradition than to clinical proof. Lion's Breath (Simhasana), a forceful exhale with an open mouth and stuck-out tongue, is centuries old and traditionally said to release tension, but it has not been isolated and tested the way breath ratios or affect labeling have.\\nWhat is well supported separately: a long exhale calms the nervous system, and naming a feeling calms the amygdala. Vocalising while exhaling plausibly does both at once, even without a dedicated study proving the combination.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-yoga-general\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Yoga & stress, honest\",\n"
  "  \"body\": \"Across the wider research on yoga, NCCIH's own summary is measured: yoga may help with anxiety or depressive symptoms tied to difficult life situations, but research specifically on anxiety disorders, depression, or PTSD is described as mildly positive and still preliminary.\\nThat is the honest ceiling for this whole edition: yoga is a reasonable, low-risk tool for everyday stress, not a proven treatment for a diagnosed condition.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-qr-nccih\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"NCCIH on yoga\",\n"
  "  \"text\": \"The federal government's own plain-language summary of what research does and does not support for yoga.\",\n"
  "  \"url\": \"https://www.nccih.nih.gov/health/yoga\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-p-sigh-study\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"The Stanford trial\",\n"
  "  \"question\": \"How many participants were in the 2023 cyclic sighing trial, and for how long did they practise?\",\n"
  "  \"answer\": \"114 participants, five minutes a day, for one month, comparing four different daily practices.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-p-labeling\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Why naming helps\",\n"
  "  \"question\": \"According to the 2007 UCLA study, what happened in the brain when people labeled a feeling in words?\",\n"
  "  \"answer\": \"Activity dropped in the amygdala, the brain's threat-alarm region, while a language-related prefrontal area became more active.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"anger-p-men\",\n"
  "  \"section\": \"anger\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Who is missing\",\n"
  "  \"question\": \"In the 2017 U.S. national survey, roughly what share of men practised yoga, versus women?\",\n"
  "  \"answer\": \"About 8.6% of men versus 19.8% of women, more than a two-to-one gap that this edition's own audience is built around closing.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"prac-structure\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The four-part shape\",\n"
  "  \"body\": \"Invisible Chaos Yoga runs a compact 45-minute session in four parts: Open (an Om over a low drone, arriving), Ground (gravity work, weight doing the stretching), Release (breath with voice allowed in it), Close (a short return to stillness).\\nThe shape is fixed even when the music, the channel, and the exact poses change session to session. Consistency in structure is what lets the content inside it stay improvised.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"prac-before-after\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Before and after class\",\n"
  "  \"body\": \"Arrive with an empty-ish stomach, roughly two hours since a full meal. A few minutes of quiet before class settles the nervous system before it is asked to do anything.\\nAfter class, a short Savasana (lying still) lets the body register what the session just did, rather than rushing straight from effort into the next task of the day.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"prac-home\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Minimal home practice\",\n"
  "  \"body\": \"Ten minutes is enough to matter: a couple of gravity-work stretches, one physiological sigh cycle, and a minute of stillness at the end. Frequency beats duration, a short daily habit outperforms one long weekly session for most stress-related benefits.\\nThe home version does not need music, a mat, or a plan. It needs ten minutes and permission to do it badly.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"prac-45min\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Why 45 minutes\",\n"
  "  \"body\": \"Long enough to move through all four parts of the structure without rushing any of them, short enough to fit before or after a work day without becoming its own obstacle. It is a deliberately modest length, chosen for an audience that has never set foot in a studio, not for people already committed to a 90-minute practice.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"prac-c-structure\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Session shape\",\n"
  "  \"items\": [\n"
  "   \"Open: Om over a low drone\",\n"
  "   \"Ground: gravity does the work\",\n"
  "   \"Release: breath, voice allowed\",\n"
  "   \"Close: short return to stillness\",\n"
  "   \"Same shape, different music each time\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"prac-p-length\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Session length\",\n"
  "  \"question\": \"How long is a standard Invisible Chaos Yoga session, and how many parts does it have?\",\n"
  "  \"answer\": \"45 minutes, in four parts: Open, Ground, Release, Close.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"prac-p-home\",\n"
  "  \"section\": \"practice\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Minimum practice\",\n"
  "  \"question\": \"What three things does the minimal home practice actually require?\",\n"
  "  \"answer\": \"Ten minutes, a willingness to do it badly, and nothing else. No mat, no music, and no plan are required.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"roots-eight-limbs\",\n"
  "  \"section\": \"roots\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The eight limbs\",\n"
  "  \"body\": \"Traditionally, the Yoga Sutras describe yoga as eight linked limbs, of which physical postures (asana) are only one: ethical restraints (yama), personal observances (niyama), postures (asana), breath control (pranayama), sense withdrawal (pratyahara), concentration (dharana), meditation (dhyana), and absorption (samadhi).\\nIn most modern classes, including this one, only asana and pranayama are directly practised; the rest remain background philosophy rather than instruction.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"roots-yamas\",\n"
  "  \"section\": \"roots\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The five yamas\",\n"
  "  \"body\": \"Traditionally listed as ethical restraints, ways of relating to others: non-harming (ahimsa), truthfulness (satya), non-stealing (asteya), moderation (brahmacharya), and non-grasping (aparigraha).\\nThese are presented here as tradition, not as rules this edition enforces or verifies. Different schools translate and weight them differently.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"roots-niyamas\",\n"
  "  \"section\": \"roots\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The five niyamas\",\n"
  "  \"body\": \"The counterpart to the yamas: personal observances rather than social ones. Cleanliness (saucha), contentment (santosha), disciplined effort (tapas), self-study (svadhyaya), and surrender to something larger than oneself (ishvara pranidhana).\\nAgain, tradition rather than physiology: useful as a lens on the practice, not as a claim about what the body or brain is doing.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"roots-om\",\n"
  "  \"section\": \"roots\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Why open on Om\",\n"
  "  \"body\": \"In tradition, Om (or Aum) is considered the primordial sound underlying all others, and opening or closing a practice with it is common across many schools of yoga.\\nInvisible Chaos Yoga borrows the opening for a practical reason as much as a traditional one: a single sustained tone over a low drone gives the room one shared point to arrive at together, before the music and movement start.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"roots-p-limbs\",\n"
  "  \"section\": \"roots\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Counting the limbs\",\n"
  "  \"question\": \"How many limbs does the traditional Ashtanga framework describe, and which two do most classes actually teach?\",\n"
  "  \"answer\": \"Eight limbs in total; asana (posture) and pranayama (breath) are the two most classes actually practise.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"roots-p-yamas\",\n"
  "  \"section\": \"roots\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Yamas, broadly\",\n"
  "  \"question\": \"Broadly speaking, are the yamas about how you treat yourself or how you relate to others?\",\n"
  "  \"answer\": \"Others. They are traditionally framed as ethical restraints governing conduct toward other people; the niyamas cover the personal, inward half.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"teach-cueing\",\n"
  "  \"section\": \"teaching\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Cueing that lands\",\n"
  "  \"body\": \"Short cues beat long ones, especially mid-breath. \\\"Soften the knees\\\" lands faster than a paragraph about knee mechanics. Link cues to the breath where possible, an inhale to lengthen, an exhale to fold or release, since students are already listening to their own breath.\\nOne instruction at a time. A student adjusting three things at once usually manages none of them well.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"teach-consent\",\n"
  "  \"section\": \"teaching\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Hands-on adjustment\",\n"
  "  \"body\": \"Any physical adjustment needs consent, asked for before class or checked in the moment, never assumed. \\\"Would you like a hands-on adjustment today?\\\" at the start of class, honoured individually, covers most situations without singling anyone out mid-pose.\\nA verbal or visual cue for adjustment (a card, a nod) works for students who would rather not speak up in front of the room.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"teach-safety\",\n"
  "  \"section\": \"teaching\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Safety, not diagnosis\",\n"
  "  \"body\": \"An instructor is not a doctor, and should not act like one. When a student reports pain or mentions a condition, the right response is to offer a modification or an alternative, and to suggest they speak with a medical professional if the issue is ongoing, not to guess a cause or promise a fix.\\nGeneral contraindications worth naming aloud at the start of a class: pregnancy, high blood pressure, recent injury. \\\"Adapt, or sit this one out\\\" is always an acceptable instruction to give.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"teach-planning\",\n"
  "  \"section\": \"teaching\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Planning a class\",\n"
  "  \"body\": \"Work backward from the closing state you want students to leave in, then choose poses and breath work that build toward it rather than a random sequence of favourites. Warm the body before asking for its deepest range. Balance effort with rest across the session, not just at the very end.\\nA class plan is a sketch, not a script: what a room actually needs on the day should always outrank what was written the night before.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"teach-c-preclass\",\n"
  "  \"section\": \"teaching\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Before the doors open\",\n"
  "  \"items\": [\n"
  "   \"Room temperature checked\",\n"
  "   \"Music channel tested and ready\",\n"
  "   \"Session timer set\",\n"
  "   \"Props available and visible\",\n"
  "   \"Adjustment consent will be asked\",\n"
  "   \"A closing state in mind\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"teach-p-consent\",\n"
  "  \"section\": \"teaching\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Before you touch\",\n"
  "  \"question\": \"What should an instructor always do before giving a hands-on physical adjustment?\",\n"
  "  \"answer\": \"Ask for consent explicitly, ahead of time or in the moment, and never assume it from silence.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"teach-p-painreport\",\n"
  "  \"section\": \"teaching\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Reported pain\",\n"
  "  \"question\": \"If a student mentions ongoing pain, what should an instructor do, and what should they avoid doing?\",\n"
  "  \"answer\": \"Offer a modification and suggest a professional if it persists. Avoid diagnosing, naming a cause, or promising the practice will fix it.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"teaching\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Yog!a Lite\",\n"
  "  \"body\": \"Written by Julien and Claude the AI\"\n"
  " }\n"
  "]\n";
const char CA_EDITION[] =
  "{\n"
  " \"name\": \"C@tlas\",\n"
  " \"slug\": \"catlas\",\n"
  " \"version\": \"1.0.1\",\n"
  " \"lang\": \"en\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"sources\": [\n"
  "  \"Herbst C.T. et al. (2023). Domestic cat larynges can produce purring frequencies without neural input. Current Biology 33, 4727-4732. doi:10.1016/j.cub.2023.09.014\",\n"
  "  \"von Muggenthaler E. (2001). The felid purr: a healing mechanism? Journal of the Acoustical Society of America 110(5), 2666 (conference abstract).\",\n"
  "  \"Remmers J.E., Gautier H. (1972). Neural and mechanical mechanisms of feline purring. Respiration Physiology 16, 351-361.\",\n"
  "  \"Heffner R.S., Heffner H.E. (1985). Hearing range of the domestic cat. Hearing Research 19, 85-88.\",\n"
  "  \"Humphrey T. et al. (2020). The role of cat eye narrowing movements in cat-human communication. Scientific Reports 10, 16503.\",\n"
  "  \"Vitale K.R., Behnke A.C., Udell M.A.R. (2019). Attachment bonds between domestic cats and humans. Current Biology 29, R864-R865.\",\n"
  "  \"McComb K. et al. (2009). The cry embedded within the purr. Current Biology 19, R507-R508.\",\n"
  "  \"Cafazzo S., Natoli E. (2009). The social function of tail up in the domestic cat. Behavioural Processes 80, 60-66.\",\n"
  "  \"Driscoll C.A. et al. (2007). The Near Eastern origin of cat domestication. Science 317, 519-523.\",\n"
  "  \"Vigne J.-D. et al. (2004). Early taming of the cat in Cyprus. Science 304, 259.\",\n"
  "  \"Ottoni C. et al. (2017). The palaeogenetics of cat dispersal in the ancient world. Nature Ecology & Evolution 1, 0139.\",\n"
  "  \"Whitney W.O., Mehlhaff C.J. (1987). High-rise syndrome in cats. Journal of the American Veterinary Medical Association 191, 1399-1403.\",\n"
  "  \"ASPCA Animal Poison Control Center: toxic and non-toxic plants; people foods to avoid; lily toxicity guidance. aspca.org\",\n"
  "  \"International Cat Care: guidance on permethrin, paracetamol, lilies and cat behaviour. icatcare.org\",\n"
  "  \"MSD Veterinary Manual: normal physiological values for cats.\",\n"
  "  \"Cornell Feline Health Center: urethral obstruction, feeding and lactose, toxoplasmosis.\",\n"
  "  \"US Centers for Disease Control and Prevention (CDC): Toxoplasmosis, prevention and risk from cats.\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"senses\",\n"
  "   \"title\": \"Senses\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"body\",\n"
  "   \"title\": \"Body\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"body-language\",\n"
  "   \"title\": \"Body language\",\n"
  "   \"order\": 3\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"health\",\n"
  "   \"title\": \"Health basics\",\n"
  "   \"order\": 4\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"history\",\n"
  "   \"title\": \"History\",\n"
  "   \"order\": 5\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"myths\",\n"
  "   \"title\": \"Myths\",\n"
  "   \"order\": 6\n"
  "  }\n"
  " ]\n"
  "}\n";
const char CA_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"senses-dim-light\",\n"
  "  \"section\": \"senses\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Seeing in dim light\",\n"
  "  \"body\": \"Cats see well in light too dim for us. Their retinas are rich in rods, the cells that work in low light, and a mirror-like layer behind the retina, the **tapetum lucidum**, sends light back through for a second chance.\\n\\nThat layer is why cat eyes glow in headlights. The trade-off is daytime detail: cats see fine detail less sharply than we do, and very close objects look blurry.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"senses-colour\",\n"
  "  \"section\": \"senses\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Colour, cat style\",\n"
  "  \"body\": \"Cats are not colour-blind, but their colour world is narrower than ours. They have two main types of cone cells where we have three, so they see mostly blues and yellow-greens.\\n\\nReds and oranges look dull, closer to grey or brown. What cats lack in colour they make up for in motion: they are very good at spotting small, fast movements, which suits a hunter of mice and birds.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"senses-hearing\",\n"
  "  \"section\": \"senses\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Hearing\",\n"
  "  \"body\": \"Cat hearing is among the broadest of any mammal. At 70 dB, cats hear from about 48 Hz up to about **85 kHz** (Heffner & Heffner, 1985). Humans stop near 20 kHz.\\n\\nThat high range reaches the squeaks of mice and other rodents. Each outer ear is moved by about 30 muscles and can turn on its own, which helps a cat pinpoint where a sound comes from.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"senses-whiskers\",\n"
  "  \"section\": \"senses\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Whiskers\",\n"
  "  \"body\": \"Whiskers, or vibrissae, are thick hairs set deep in follicles full of nerves. They pick up touch and even faint air currents.\\n\\nCats have them on the muzzle, above the eyes, on the cheeks and on the back of the front legs. They help in the dark, in narrow spaces, and when prey is too close to the face to see clearly. Whiskers should never be trimmed.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"senses-second-nose\",\n"
  "  \"section\": \"senses\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The second nose\",\n"
  "  \"body\": \"Besides their normal sense of smell, cats have a vomeronasal organ, also called **Jacobson's organ**. It sits in the roof of the mouth, behind the front teeth, and detects chemical signals from other animals.\\n\\nTo use it, a cat may open its mouth slightly and pull back its upper lip, as if grimacing. This is the flehmen response: it draws scent toward the organ. It is not disgust.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"senses-p-glow\",\n"
  "  \"section\": \"senses\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Glowing eyes\",\n"
  "  \"question\": \"What makes cat eyes glow in the dark?\",\n"
  "  \"answer\": \"The tapetum lucidum, a reflective layer behind the retina. It sends light back through the retina, giving the eye a second chance to detect it.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"senses-p-hearing\",\n"
  "  \"section\": \"senses\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Top of the range\",\n"
  "  \"question\": \"Up to roughly what frequency can cats hear a 70 dB sound?\",\n"
  "  \"answer\": \"About 85 kHz, far above the human limit of about 20 kHz (Heffner & Heffner, 1985).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"senses-p-flehmen\",\n"
  "  \"section\": \"senses\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"The grimace\",\n"
  "  \"question\": \"What is the flehmen response?\",\n"
  "  \"answer\": \"An open-mouthed, lip-curled expression that draws scent toward the vomeronasal organ in the roof of the mouth. It is sniffing, not disgust.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"body-purr-how\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"How the purr is made\",\n"
  "  \"body\": \"The purr comes from the larynx. For decades the accepted view was that it needs fast, rhythmic twitches of the laryngeal muscles, driven by the brain, on both the in-breath and the out-breath.\\n\\nIn 2023, researchers showed that a cat larynx can produce purr-like sounds at 20–30 Hz with no nerve input at all, thanks to **pads of tissue in the vocal folds** (Herbst et al., 2023). Both mechanisms may work together in a living cat.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"body-purr-freq\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Purr frequencies\",\n"
  "  \"body\": \"A domestic cat's purr is very low: its base frequency usually sits around **20–30 Hz**, below the lowest notes of most human voices (Herbst et al., 2023).\\n\\nAcross the cat family, purrs from about 20 to 150 Hz have been reported (von Muggenthaler, 2001). The purr runs through breathing in and out with barely a pause, which is why it is felt as much as heard.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"body-righting\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The righting reflex\",\n"
  "  \"body\": \"A falling cat turns to land feet first. The reflex appears at about 3 to 4 weeks of age and is mature by about 6 to 7 weeks.\\n\\nA very flexible spine and a small, loosely attached collarbone let the cat rotate the front of the body first, then the back. The turn needs time and height, so a short fall can still end badly, and a long one can cause serious injuries.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"body-claws\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Claws and scratching\",\n"
  "  \"body\": \"Cat claws are retractable: at rest they stay sheathed, which keeps them sharp. Scratching is normal and useful. It pulls off worn outer claw layers, stretches the body, and leaves scent from glands in the paws, plus a visible mark.\\n\\nThe practical answer is not to stop scratching but to redirect it: a sturdy, tall post or mat in a spot the cat already likes.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"body-sleep\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Sleep and rhythm\",\n"
  "  \"body\": \"Adult cats often sleep or doze 12 to 16 hours a day, usually in many short sessions. Much of that is light sleep, ready to wake in an instant.\\n\\nCats are **crepuscular**: naturally most active at dawn and dusk, when their small prey is active too. That is why a household cat may be at its liveliest early in the morning.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"body-p-purr\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Purr pitch\",\n"
  "  \"question\": \"Around what frequency does a domestic cat's purr usually sit?\",\n"
  "  \"answer\": \"Around 20–30 Hz, below the lowest notes of most human voices (Herbst et al., 2023).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"body-p-active\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Dawn and dusk\",\n"
  "  \"question\": \"When are cats naturally most active?\",\n"
  "  \"answer\": \"At dawn and dusk. Cats are crepuscular, active when their small prey is.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"body-p-scratch\",\n"
  "  \"section\": \"body\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Why scratch?\",\n"
  "  \"question\": \"Why do cats scratch furniture and posts?\",\n"
  "  \"answer\": \"To shed worn claw layers, stretch, and leave scent and visible marks. It is normal behaviour: offer a sturdy post rather than trying to stop it.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-tail\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Tail signals\",\n"
  "  \"body\": \"A tail held straight up, sometimes with a slight hook at the tip, usually signals a friendly approach. Studies of free-roaming cats link it to greeting and rubbing (Cafazzo & Natoli, 2009).\\n\\nA puffed tail means alarm. A low or tucked tail suggests fear or unease. A tail that lashes or thumps usually means irritation or high arousal. Always read the tail together with ears, eyes and posture.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-ears\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Ears\",\n"
  "  \"body\": \"Ears pointing forward usually mean interest or calm attention. Ears turned out to the side, like aeroplane wings, suggest unease.\\n\\nEars flattened back against the head signal fear or a defensive mood: a cat that looks like this is best left alone. Ears also swivel independently toward sounds, so a single twitch may just mean the cat is listening.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-slow-blink\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The slow blink\",\n"
  "  \"body\": \"A slow blink is a series of half-blinks, then narrowed or closed eyes. A 2020 study found that cats narrowed their eyes more after their owners slow-blinked at them, and approached a stranger's hand more readily after that person slow-blinked (Humphrey et al., 2020).\\n\\nTo try it: narrow your eyes as in a relaxed smile, then close them for a second or two.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-purr-meaning\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"A purr is not a smile\",\n"
  "  \"body\": \"Purring often means contentment, but not always. Cats also purr when stressed, unwell, injured or giving birth, and sometimes near the end of life.\\n\\nSo the purr is best read as a signal in context, not as proof of happiness. A purring cat that is hiding, not eating or holding itself stiffly may need help, not reassurance.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-meow\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Meows are for people\",\n"
  "  \"body\": \"Kittens meow to their mother, but adult cats rarely meow at one another. Adult meowing is mostly aimed at humans.\\n\\nMany owners notice that their cat has particular calls for particular needs, and that these vary from one cat and household to another. One study found that some cats hide a higher cry inside a purr when asking for food, a sound people rate as more urgent (McComb et al., 2009).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-p-tail\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Tail up\",\n"
  "  \"question\": \"What does a cat's tail held straight up usually signal?\",\n"
  "  \"answer\": \"A friendly approach or greeting. In studies of free-roaming cats, tail-up is linked to rubbing and other friendly contact.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-p-blink\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"How to slow blink\",\n"
  "  \"question\": \"How do you slow-blink at a cat?\",\n"
  "  \"answer\": \"Narrow your eyes as in a relaxed smile, then close them for a second or two. Many cats answer with their own eye narrowing.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-p-purr\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Purr check\",\n"
  "  \"question\": \"Does purring always mean a cat is happy?\",\n"
  "  \"answer\": \"No. Cats also purr when stressed, ill, injured or giving birth. Read the context and the rest of the body.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-p-solicit\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"The asking purr\",\n"
  "  \"question\": \"What is a solicitation purr?\",\n"
  "  \"answer\": \"A purr with a higher-pitched cry mixed in, often used when asking for food. People rate it as more urgent (McComb et al., 2009).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"bl-qr-catlas\",\n"
  "  \"section\": \"body-language\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Play Catlas\",\n"
  "  \"text\": \"Catlas: a small cat on a small planet. You can't steer it, only earn its trust. A free InvisibleChaos game, no account.\",\n"
  "  \"url\": \"https://ic-catlas.netlify.app/\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-lilies\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Lilies are deadly\",\n"
  "  \"body\": \"True lilies (Lilium) and daylilies (Hemerocallis) can cause **kidney failure** in cats. Every part is toxic: flowers, leaves, stems, pollen groomed off the fur, even water from the vase.\\n\\nEarly signs include vomiting, drooling and lethargy. Treatment works if it starts early and is often fatal when delayed beyond about 18 hours. Any contact means: see a vet now. Safest of all is no lilies at home.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-foods\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Foods to keep away\",\n"
  "  \"body\": \"Onions, garlic, leeks and chives damage red blood cells, raw or cooked, even as powder in sauces. Chocolate and caffeine contain stimulants cats handle badly. Alcohol and raw yeast dough are dangerous too.\\n\\nGrapes and raisins are proven kidney risks in dogs and are best kept away from cats as well. If your cat eats any of these, call a vet or a poison line. Not veterinary advice.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-medicines\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Human medicines\",\n"
  "  \"body\": \"Many human medicines are dangerous to cats. **Paracetamol** (acetaminophen) is the classic case: cats cannot process it properly, and even a single tablet can kill.\\n\\nIbuprofen and similar painkillers can damage the stomach and kidneys. Keep medicines shut away, pick up dropped pills at once, and never give a cat any medicine a vet has not prescribed. If it happens: see a vet now.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-flea\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Dog flea products\",\n"
  "  \"body\": \"Some flea treatments made for dogs contain **permethrin**, which is highly toxic to cats. A dog-only spot-on used on a cat, or even close contact with a freshly treated dog, can cause twitching, tremors and seizures.\\n\\nOnly use products labelled for cats, and ask a vet or pharmacist if unsure. Signs after exposure mean: see a vet now.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-vitals\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Normal vital signs\",\n"
  "  \"body\": \"Typical values for a healthy adult cat at rest: body temperature about 38.1–39.2 °C, heart rate about 140–220 beats a minute, and 20–30 breaths a minute.\\n\\nA sleeping cat usually takes fewer than 30 breaths a minute. Faster breathing at rest, visible effort, or breathing with an open mouth is a reason to see a vet, urgently if it is new.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-c-vet-now\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"See a vet now\",\n"
  "  \"items\": [\n"
  "   \"Straining to pee, little or nothing comes out\",\n"
  "   \"Breathing with open mouth, or panting\",\n"
  "   \"Any contact with lilies\",\n"
  "   \"Ate human medicine or dog flea product\",\n"
  "   \"Seizure or collapse\",\n"
  "   \"Back legs suddenly weak, cold or dragging\",\n"
  "   \"Repeated vomiting, or blood in vomit\",\n"
  "   \"Pale, blue or yellow gums\",\n"
  "   \"Fall or road accident, even if it seems fine\",\n"
  "   \"No food at all for over 24 hours\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-c-vet-soon\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Book a vet visit\",\n"
  "  \"items\": [\n"
  "   \"Drinking or peeing more than usual\",\n"
  "   \"Weight loss\",\n"
  "   \"Eating less, or dropping food\",\n"
  "   \"Hiding more, playing less\",\n"
  "   \"Changes in litter tray habits\",\n"
  "   \"Bad breath or drooling\",\n"
  "   \"Coat unkempt, grooming less\",\n"
  "   \"Limping or stiffness\",\n"
  "   \"New lumps\",\n"
  "   \"Discharge from eyes or nose\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-c-safe-home\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"checklist\",\n"
  "  \"title\": \"Cat-safe home\",\n"
  "  \"items\": [\n"
  "   \"No lilies in the house or in bouquets\",\n"
  "   \"Medicines shut away\",\n"
  "   \"Strings, threads and hair ties put away\",\n"
  "   \"Windows and balconies screened\",\n"
  "   \"Check washer and dryer before use\",\n"
  "   \"Flea products labelled for cats only\",\n"
  "   \"Cleaning products closed and stored\",\n"
  "   \"Chewed cables covered\"\n"
  "  ]\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-p-lily\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Which part of a lily?\",\n"
  "  \"question\": \"Which part of a lily is toxic to cats?\",\n"
  "  \"answer\": \"All of it: flowers, leaves, stems, pollen, even the vase water. Any exposure is an emergency: see a vet now.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-p-block\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Straining to pee\",\n"
  "  \"question\": \"Why is a male cat straining in the litter tray with little result an emergency?\",\n"
  "  \"answer\": \"The urethra may be blocked. Urine cannot get out, and this can become fatal within a day or two. See a vet now.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-p-para\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"One tablet\",\n"
  "  \"question\": \"Can a single paracetamol tablet harm a cat?\",\n"
  "  \"answer\": \"Yes. Cats process paracetamol very poorly, and even one tablet can be fatal. Never give human medicines without a vet.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-p-temp\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Body temperature\",\n"
  "  \"question\": \"What is the normal body temperature of an adult cat?\",\n"
  "  \"answer\": \"About 38.1 to 39.2 °C, a little higher than ours.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-qr-aspca\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Poison control\",\n"
  "  \"text\": \"ASPCA Animal Poison Control: lists of toxic and non-toxic plants, and what to do if your cat ate something risky.\",\n"
  "  \"url\": \"https://www.aspca.org/pet-care/animal-poison-control\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"health-qr-icc\",\n"
  "  \"section\": \"health\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"International Cat Care\",\n"
  "  \"text\": \"A charity sharing expert advice on cat health, behaviour and welfare, for owners and vets.\",\n"
  "  \"url\": \"https://icatcare.org/\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hist-ancestor\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The wild ancestor\",\n"
  "  \"body\": \"All domestic cats descend from the Near Eastern wildcat, Felis lybica lybica, from the region of the Fertile Crescent (Driscoll et al., 2007).\\n\\nThe likely story: when people began farming and storing grain, rodents moved in, and wildcats followed the rodents. Cats that tolerated people did well. Many researchers see this as partly self-domestication rather than capture and breeding.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hist-cyprus\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"The Cyprus grave\",\n"
  "  \"body\": \"In 2004, archaeologists described a grave about **9,500 years old** at Shillourokambos, a Neolithic village on Cyprus. A cat lay buried close to a human (Vigne et al., 2004).\\n\\nCyprus had no native wildcats, so cats must have been brought over by boat. The find predates Egyptian art showing cats by some 4,000 years.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hist-egypt\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Cats of Egypt\",\n"
  "  \"body\": \"Ancient Egypt honoured cats. The goddess Bastet was shown with a cat's head, and cats were mummified in very large numbers, many as offerings.\\n\\nAncient DNA suggests cats spread in two waves: first from the Near East with early farming, then from Egypt along trade routes, including on ships (Ottoni et al., 2017). Cats kept rodents off grain, cargo and ropes.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hist-felicette\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Félicette in space\",\n"
  "  \"body\": \"On 18 October 1963, France launched a cat named Félicette on a short suborbital rocket flight. She was recovered alive, making her the first cat in space.\\n\\nFor decades she was little known. In 2019 a bronze statue of her was unveiled at the International Space University in Strasbourg, after a public fundraising campaign.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hist-tama\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Stationmaster Tama\",\n"
  "  \"body\": \"Tama was a calico cat who lived at Kishi station in Wakayama, Japan. In 2007 the railway named her stationmaster, complete with a small cap.\\n\\nVisitors came to see her, and ridership on the struggling line rose. After she died in 2015, other cats took over the post. Tama is one of many cats who became local celebrities, from ship's cats to library cats.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hist-p-ancestor\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"The ancestor\",\n"
  "  \"question\": \"From which wild cat do all house cats descend?\",\n"
  "  \"answer\": \"The Near Eastern wildcat, Felis lybica lybica, from the Fertile Crescent (Driscoll et al., 2007).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hist-p-cyprus\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Oldest cat grave\",\n"
  "  \"question\": \"Roughly how old is the cat burial found on Cyprus?\",\n"
  "  \"answer\": \"About 9,500 years, from the Neolithic village of Shillourokambos (Vigne et al., 2004).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"hist-p-space\",\n"
  "  \"section\": \"history\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"First cat in space\",\n"
  "  \"question\": \"Who was the first cat in space?\",\n"
  "  \"answer\": \"Félicette, a French cat launched on 18 October 1963 on a short suborbital flight and recovered alive.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-feet\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Always on their feet?\",\n"
  "  \"body\": \"The righting reflex is real, but it does not make cats safe from falls. Vets see many injured cats after falls from windows and balconies.\\n\\nA 1987 study of 132 such cats found about 90% survived with treatment (Whitney & Mehlhaff, 1987). Its odd finding that very high falls seemed less harmful is debated: cats killed outright rarely reach a clinic. Screen open windows.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-milk\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"A saucer of milk\",\n"
  "  \"body\": \"Milk is a classic cat image, but many adult cats digest lactose poorly. For them, cow's milk can cause an upset stomach and diarrhoea.\\n\\nKittens need their mother's milk or a kitten formula, not cow's milk. For adult cats, fresh water is all they need to drink. Any milk-based treat is best kept small and occasional.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-aloof\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Cats are aloof\",\n"
  "  \"body\": \"The cliché says cats do not bond with people. A 2019 study tested kittens and adult cats with a method used for human infants and dogs (Vitale et al., 2019).\\n\\nAbout 64% of kittens and 66% of adults were securely attached to their owner, close to the 65% seen in human infants. Cats may show attachment more quietly than dogs, but most do use their people as a source of security.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-healing\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Purrs that heal?\",\n"
  "  \"body\": \"A popular claim says purring heals bones. It rests on an overlap: purr frequencies (about 20–150 Hz) fall in a range where vibration has been studied for bone and tissue healing. The idea was proposed in a 2001 conference abstract.\\n\\n**Evidence is limited.** No study has shown that purring heals bones in cats or in people. Purring may still be calming for many owners, which is a different claim.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-nine-lives\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Nine lives\",\n"
  "  \"body\": \"Cats do not have nine lives. The saying reflects how agile and resilient they seem.\\n\\nThe number changes with language: English speaks of nine, many Spanish speakers say seven, and some Arabic and Turkish sayings give six. The differences show this is folklore, not observation. Every fall, poison or road accident is a real risk to a cat's only life.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-black-cats\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Black cats\",\n"
  "  \"body\": \"Whether black cats bring bad luck depends on where you ask. In parts of Europe and North America, a black cat crossing your path is traditionally unlucky.\\n\\nIn Britain and Japan, black cats have traditionally been seen as lucky, and sailors often welcomed them on board. None of this is evidence, only tradition. The coat colour tells you nothing about the cat's temperament.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-pregnancy\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Cats and pregnancy\",\n"
  "  \"body\": \"Toxoplasmosis is a real concern in pregnancy, but most human infections come from undercooked meat or soil, not from pet cats.\\n\\nThe eggs in cat faeces need 1 to 5 days to become infective, so a litter tray cleaned daily, ideally by someone else or with gloves, keeps the risk low. Pregnancy is not a reason to give up a cat. Ask a doctor for personal advice.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-poinsettia\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"Poinsettia panic\",\n"
  "  \"body\": \"Poinsettias have a scary reputation, but for cats they usually cause only mild irritation of the mouth and stomach, such as drooling or vomiting.\\n\\nThe real danger plant in many homes is the lily, which can cause fatal kidney failure. Worth keeping the proportions in mind: poinsettia, watch; lily, see a vet now.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-p-attach\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Secure attachment\",\n"
  "  \"question\": \"In a 2019 study, what share of adult cats were securely attached to their owners?\",\n"
  "  \"answer\": \"About 66% of adults (and 64% of kittens), close to the 65% seen in human infants (Vitale et al., 2019).\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-p-milk\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Milk or water?\",\n"
  "  \"question\": \"Is cow's milk good for adult cats?\",\n"
  "  \"answer\": \"Not really. Many adult cats digest lactose poorly, so milk can upset the stomach. Fresh water is all they need to drink.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-p-heal\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Healing purr?\",\n"
  "  \"question\": \"Is there solid evidence that a cat's purr heals bones?\",\n"
  "  \"answer\": \"No. The idea rests on a frequency overlap and a 2001 conference abstract. No study has shown purring heals bones in cats or people.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"myth-p-litter\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"Litter and pregnancy\",\n"
  "  \"question\": \"Why does cleaning the litter tray daily lower the toxoplasmosis risk?\",\n"
  "  \"answer\": \"Toxoplasma eggs in cat faeces need 1 to 5 days to become infective. Removing them daily, with gloves or by someone else, cuts the risk.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"myths\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"C@tlas\",\n"
  "  \"body\": \"Written by Julien and Claude the AI\"\n"
  " }\n"
  "]\n";

// generated from editions/<slug>/*.json: W@terfacts! Lite in EN, DE, FR (from the Editions chat, v1.0.1: the 1842 fact corrected)
const char WF_EDITION[] =
  "{\n"
  " \"name\": \"W@terfacts! Lite\",\n"
  " \"slug\": \"waterfacts-lite\",\n"
  " \"version\": \"1.0.2\",\n"
  " \"lang\": \"en\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"sources\": [\n"
  "  \"Wikipedia (EN), “Deep Sea Drilling Project”. https://en.wikipedia.org/wiki/Deep_Sea_Drilling_Project\",\n"
  "  \"Wikipedia (EN), “1966 flood of the Arno”. https://en.wikipedia.org/wiki/1966_flood_of_the_Arno\",\n"
  "  \"Wikipedia (DE), “Wassersicherstellungsgesetz”. https://de.wikipedia.org/wiki/Wassersicherstellungsgesetz\",\n"
  "  \"Wikipedia (EN), “Vajont Dam”. https://en.wikipedia.org/wiki/Vajont_Dam\",\n"
  "  \"Wikipedia (EN), “North Sea flood of 1962”. https://en.wikipedia.org/wiki/North_Sea_flood_of_1962\",\n"
  "  \"Wikipedia (EN), “Günter Litfin”. https://en.wikipedia.org/wiki/G%C3%BCnter_Litfin\",\n"
  "  \"Wikipedia (EN), “Trieste (bathyscaphe)”. https://en.wikipedia.org/wiki/Trieste_(bathyscaphe)\",\n"
  "  \"Wikipedia (EN), “Antarctic Treaty System”. https://en.wikipedia.org/wiki/Antarctic_Treaty_System\",\n"
  "  \"Wikipedia (EN), “USS Nautilus (SSN-571)”. https://en.wikipedia.org/wiki/USS_Nautilus_(SSN-571)\",\n"
  "  \"Wikipedia (EN), “The Silent World”. https://en.wikipedia.org/wiki/The_Silent_World\",\n"
  "  \"Wikipedia (EN), “Daigo Fukuryu Maru”. https://en.wikipedia.org/wiki/Daigo_Fukury%C5%AB_Maru\",\n"
  "  \"Wikipedia (EN), “North Sea flood of 1953”. https://en.wikipedia.org/wiki/North_Sea_flood_of_1953\",\n"
  "  \"Wikipedia (EN), “Alain Bombard”. https://en.wikipedia.org/wiki/Alain_Bombard\",\n"
  "  \"Wikipedia (EN), “RV Calypso”. https://en.wikipedia.org/wiki/RV_Calypso\",\n"
  "  \"Wikipedia (EN), “Short Sunderland”. https://en.wikipedia.org/wiki/Short_Sunderland\",\n"
  "  \"Wikipedia (EN), “Kon-Tiki expedition”. https://en.wikipedia.org/wiki/Kon-Tiki_expedition\",\n"
  "  \"Wikipedia (DE), “Berliner Nordsüd-S-Bahn”. https://de.wikipedia.org/wiki/Berliner_Nords%C3%BCd-S-Bahn\",\n"
  "  \"Wikipedia (EN), “Operation Chastise”. https://en.wikipedia.org/wiki/Operation_Chastise\",\n"
  "  \"Wikipedia (EN), “Black Sunday (storm)”. https://en.wikipedia.org/wiki/Black_Sunday_(storm)\",\n"
  "  \"Wikipedia (EN), “Bathysphere”. https://en.wikipedia.org/wiki/Bathysphere\",\n"
  "  \"Wikipedia (EN), “Afsluitdijk”. https://en.wikipedia.org/wiki/Afsluitdijk\",\n"
  "  \"Wikipedia (EN), “Salt March”. https://en.wikipedia.org/wiki/Salt_March\",\n"
  "  \"Wikipedia (EN), “1929 Grand Banks earthquake”. https://en.wikipedia.org/wiki/1929_Grand_Banks_earthquake\",\n"
  "  \"Wikipedia (EN), “Gertrude Ederle”. https://en.wikipedia.org/wiki/Gertrude_Ederle\",\n"
  "  \"Wikipedia (EN), “German Atlantic Expedition”. https://en.wikipedia.org/wiki/German_Atlantic_Expedition\",\n"
  "  \"Wikipedia (EN), “International Hydrographic Organization”. https://en.wikipedia.org/wiki/International_Hydrographic_Organization\",\n"
  "  \"Wikipedia (EN), “Greater Berlin Act”. https://en.wikipedia.org/wiki/Greater_Berlin_Act\",\n"
  "  \"Wikipedia (EN), “Halifax Explosion”. https://en.wikipedia.org/wiki/Halifax_Explosion\",\n"
  "  \"Wikipedia (EN), “Zuiderzee flood of 1916”. https://en.wikipedia.org/wiki/Zuiderzee_flood_of_1916\",\n"
  "  \"Wikipedia (EN), “Panama Canal”. https://en.wikipedia.org/wiki/Panama_Canal\",\n"
  "  \"Wikipedia (EN), “Titanic”. https://en.wikipedia.org/wiki/Titanic\",\n"
  "  \"Wikipedia (EN), “1910 Great Flood of Paris”. https://en.wikipedia.org/wiki/1910_Great_Flood_of_Paris\",\n"
  "  \"Wikipedia (EN), “Teltow Canal”. https://en.wikipedia.org/wiki/Teltow_Canal\",\n"
  "  \"Wikipedia (EN), “Salton Sea”. https://en.wikipedia.org/wiki/Salton_Sea\",\n"
  "  \"Wikipedia (EN), “1900 Galveston hurricane”. https://en.wikipedia.org/wiki/1900_Galveston_hurricane\",\n"
  "  \"Wikipedia (EN), “Belgian Antarctic Expedition”. https://en.wikipedia.org/wiki/Belgian_Antarctic_Expedition\",\n"
  "  \"Wikipedia (EN), “Fram”. https://en.wikipedia.org/wiki/Fram\",\n"
  "  \"Wikipedia (DE), “Straßenbrunnen in Berlin”. https://de.wikipedia.org/wiki/Stra%C3%9Fenbrunnen_in_Berlin\",\n"
  "  \"Wikipedia (EN), “1892 Hamburg cholera epidemic”. https://en.wikipedia.org/wiki/1892_Hamburg_cholera_epidemic\",\n"
  "  \"Wikipedia (EN), “Johnstown Flood”. https://en.wikipedia.org/wiki/Johnstown_Flood\",\n"
  "  \"Wikipedia (EN), “Robert Koch”. https://en.wikipedia.org/wiki/Robert_Koch\",\n"
  "  \"Wikipedia (EN), “1883 eruption of Krakatoa”. https://en.wikipedia.org/wiki/1883_eruption_of_Krakatoa\",\n"
  "  \"Wikipedia (EN), “Challenger expedition”. https://en.wikipedia.org/wiki/Challenger_expedition\",\n"
  "  \"Wikipedia (DE), “James Hobrecht”. https://de.wikipedia.org/wiki/James_Hobrecht\",\n"
  "  \"Wikipedia (EN), “Twenty Thousand Leagues Under the Seas”. https://en.wikipedia.org/wiki/Twenty_Thousand_Leagues_Under_the_Seas\",\n"
  "  \"Wikipedia (EN), “Suez Canal”. https://en.wikipedia.org/wiki/Suez_Canal\",\n"
  "  \"Wikipedia (EN), “1868 Arica earthquake”. https://en.wikipedia.org/wiki/1868_Arica_earthquake\",\n"
  "  \"Wikipedia (EN), “The Water-Babies, A Fairy Tale for a Land Baby”. https://en.wikipedia.org/wiki/The_Water-Babies,_A_Fairy_Tale_for_a_Land_Baby\",\n"
  "  \"Wikipedia (EN), “Robert FitzRoy”. https://en.wikipedia.org/wiki/Robert_FitzRoy\",\n"
  "  \"Wikipedia (EN), “Great Stink”. https://en.wikipedia.org/wiki/Great_Stink\",\n"
  "  \"Wikipedia (EN), “Berliner Wasserbetriebe”. https://en.wikipedia.org/wiki/Berliner_Wasserbetriebe\",\n"
  "  \"Wikipedia (EN), “Matthew Fontaine Maury”. https://en.wikipedia.org/wiki/Matthew_Fontaine_Maury\",\n"
  "  \"Wikipedia (EN), “1854 Broad Street cholera outbreak”. https://en.wikipedia.org/wiki/1854_Broad_Street_cholera_outbreak\",\n"
  "  \"Wikipedia (EN), “Moby-Dick”. https://en.wikipedia.org/wiki/Moby-Dick\",\n"
  "  \"Wikipedia (EN), “Ignaz Semmelweis”. https://en.wikipedia.org/wiki/Ignaz_Semmelweis\",\n"
  "  \"Wikipedia (EN), “Croton Aqueduct”. https://en.wikipedia.org/wiki/Croton_Aqueduct\",\n"
  "  \"Wikipedia (EN), “Louis Agassiz”. https://en.wikipedia.org/wiki/Louis_Agassiz\",\n"
  "  \"Wikipedia (EN), “Second voyage of HMS Beagle”. https://en.wikipedia.org/wiki/Second_voyage_of_HMS_Beagle\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"berlin\",\n"
  "   \"title\": \"Berlin\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"ocean\",\n"
  "   \"title\": \"Ocean\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"world\",\n"
  "   \"title\": \"World\",\n"
  "   \"order\": 3\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"chronicle\",\n"
  "   \"title\": \"Chronicle\",\n"
  "   \"order\": 4\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"test\",\n"
  "   \"title\": \"Test yourself\",\n"
  "   \"order\": 5\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"more\",\n"
  "   \"title\": \"More\",\n"
  "   \"order\": 6\n"
  "  }\n"
  " ]\n"
  "}\n";
const char WF_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"wf-1965\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1965 · Emergency wells\",\n"
  "  \"body\": \"On 24 August West Germany enacts its Water Security Act. It is the legal basis for the federal emergency wells that still stand in Berlin’s streets today.\\nSource: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1961\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1961 · Humboldthafen\",\n"
  "  \"body\": \"On 24 August Günter Litfin tries to swim across the Humboldthafen to West Berlin. He is shot in the water, the first person shot dead at the Berlin Wall.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1948\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1948 · Salt by air\",\n"
  "  \"body\": \"During the Berlin Airlift, British flying boats land on the Havel near Gatow. Their hulls are built to resist salt water, so they carry the city’s salt.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1945\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1945 · Flooded tunnel\",\n"
  "  \"body\": \"In the first days of May the roof of the S-Bahn tunnel under the Landwehrkanal is blown up. Canal water floods the north-south tunnel and much of the U-Bahn. Who gave the order is still disputed.\\nSource: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1920\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1920 · Greater Berlin\",\n"
  "  \"body\": \"On 1 October Greater Berlin is born. Overnight the city gains the Müggelsee, the Tegeler See and the Havel lakes.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1906\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1906 · Teltow Canal\",\n"
  "  \"body\": \"On 2 June the Teltow Canal opens, linking the Havel with the Spree river system across the south of Berlin.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1894\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1894 · Street pumps\",\n"
  "  \"body\": \"The first of Berlin’s new cast-iron street pumps go up, designed by Otto Stahn and cast in Lauchhammer. By 1897 some 320 of them stand in the city.\\nSource: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1890\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1890 · Pump contest\",\n"
  "  \"body\": \"Berlin holds a competition for a new street pump. The winning design by Otto Stahn will soon stand on hundreds of pavements.\\nSource: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1873\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1873 · Hobrecht sewers\",\n"
  "  \"body\": \"Berlin begins building James Hobrecht’s radial sewer system, which carries the city’s waste out to sewage farms far beyond its edges.\\nSource: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1856\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1856 · Waterworks\",\n"
  "  \"body\": \"Berlin’s first waterworks opens at the Stralauer Tor, built by an English company. Water now flows through pipes into the first houses.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1968\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1968 · Deep drilling\",\n"
  "  \"body\": \"The drilling ship Glomar Challenger begins the Deep Sea Drilling Project. Its cores from the ocean floor soon confirm that the seafloor is spreading apart.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1960\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1960 · Challenger Deep\",\n"
  "  \"body\": \"On 23 January the bathyscaphe Trieste, with Jacques Piccard and Don Walsh aboard, reaches the bottom of the Challenger Deep, almost 11,000 metres down.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1958\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1958 · Under the Pole\",\n"
  "  \"body\": \"On 3 August the submarine USS Nautilus becomes the first vessel to pass beneath the North Pole, travelling under the Arctic ice.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1956\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1956 · Silent World\",\n"
  "  \"body\": \"The Silent World, filmed underwater by Jacques-Yves Cousteau and Louis Malle, wins the Palme d’Or in Cannes.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1952\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1952 · Bombard\",\n"
  "  \"body\": \"Alain Bombard crosses the Atlantic alone in a rubber dinghy, living on fish and small amounts of seawater, to prove that castaways can survive at sea.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1950\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1950 · Calypso\",\n"
  "  \"body\": \"The former minesweeper Calypso is bought and leased to Jacques-Yves Cousteau for a symbolic one franc a year. It becomes one of the most famous research ships in the world.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1947\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1947 · Kon-Tiki\",\n"
  "  \"body\": \"Thor Heyerdahl and five companions drift 101 days on the balsa raft Kon-Tiki, from Peru to a Polynesian reef.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1934\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1934 · Bathysphere\",\n"
  "  \"body\": \"On 15 August William Beebe and Otis Barton descend in their steel bathysphere to 923 metres off Bermuda, deeper than anyone before them.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1929\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1929 · Cut cables\",\n"
  "  \"body\": \"On 18 November an earthquake off Newfoundland sets off an underwater landslide that snaps transatlantic telegraph cables one after another, and a tsunami hits the coast.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1925\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1925 · Meteor\",\n"
  "  \"body\": \"The German research ship Meteor sets out to survey the South Atlantic. With an echo sounder it maps the ocean floor as never before.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1921\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1921 · Sea charts\",\n"
  "  \"body\": \"The International Hydrographic Bureau is founded in Monaco, so that the world’s sea charts finally speak the same language.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1912\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1912 · Titanic\",\n"
  "  \"body\": \"In the night of 14 to 15 April the Titanic strikes an iceberg and sinks in the North Atlantic. More than 1,500 people die in water close to freezing.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1897\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1897 · Belgica\",\n"
  "  \"body\": \"The Belgica sails for Antarctica. Trapped in the pack ice, her crew become the first people to spend a winter in the Antarctic.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1896\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1896 · Fram\",\n"
  "  \"body\": \"Nansen’s ship Fram returns to Norway after drifting for three years frozen into the Arctic ice. The drift proves that a current crosses the polar sea.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1875\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1875 · Record depth\",\n"
  "  \"body\": \"HMS Challenger sounds more than 8,000 metres in the Mariana Trench, the greatest ocean depth known at the time.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1872\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1872 · Challenger\",\n"
  "  \"body\": \"On 21 December HMS Challenger leaves Portsmouth on the first great voyage devoted entirely to studying the oceans.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1870\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1870 · Nemo\",\n"
  "  \"body\": \"Jules Verne’s Twenty Thousand Leagues Under the Seas appears as a book. Captain Nemo’s Nautilus sends generations of readers under water.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1855\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1855 · Maury\",\n"
  "  \"body\": \"Matthew Fontaine Maury publishes The Physical Geography of the Sea, often called the first textbook of modern oceanography.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1851\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1851 · Moby-Dick\",\n"
  "  \"body\": \"Herman Melville publishes Moby-Dick, the story of Captain Ahab’s hunt for a white whale.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1835\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1835 · Galápagos\",\n"
  "  \"body\": \"The Beagle reaches the Galápagos Islands. Darwin notices that each island seems to have its own mockingbirds and giant tortoises.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1959\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1959 · Antarctica\",\n"
  "  \"body\": \"On 1 December twelve nations sign the Antarctic Treaty, keeping the continent for peace and science. Its ice holds about 70 percent of the world’s fresh water.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1932\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1932 · Afsluitdijk\",\n"
  "  \"body\": \"On 28 May the Dutch close the Afsluitdijk. The saltwater Zuiderzee becomes a freshwater lake, the IJsselmeer.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1930\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1930 · Salt March\",\n"
  "  \"body\": \"Gandhi walks for 24 days to the sea at Dandi and makes salt from seawater, defying the British salt monopoly.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1926\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1926 · Channel swim\",\n"
  "  \"body\": \"On 6 August Gertrude Ederle becomes the first woman to swim the English Channel, and beats the men’s record by almost two hours.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1914\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1914 · Panama Canal\",\n"
  "  \"body\": \"On 15 August the Panama Canal opens. Ships no longer need to sail around South America to go from the Atlantic to the Pacific.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1905\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1905 · Salton Sea\",\n"
  "  \"body\": \"The Colorado River breaks through an irrigation canal and pours into a desert basin in California for almost two years. The Salton Sea is born.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1892\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1892 · Hamburg cholera\",\n"
  "  \"body\": \"Cholera kills around 8,600 people in Hamburg, which drinks unfiltered water from the Elbe. Neighbouring Altona filters its water and is largely spared.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1884\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1884 · Cholera germ\",\n"
  "  \"body\": \"Working in Calcutta, the Berlin doctor Robert Koch identifies the cholera bacterium and shows that it spreads through contaminated water.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1869\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1869 · Suez Canal\",\n"
  "  \"body\": \"On 17 November the Suez Canal opens, joining the Mediterranean and the Red Sea.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1863\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1863 · Water-Babies\",\n"
  "  \"body\": \"Charles Kingsley’s The Water-Babies appears as a book: a young chimney sweep falls into a river and becomes a creature of the river and the sea.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1861\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1861 · First forecast\",\n"
  "  \"body\": \"On 1 August The Times prints the first public weather forecasts, drawn up by Robert FitzRoy, former captain of the Beagle.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1858\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1858 · Great Stink\",\n"
  "  \"body\": \"In a hot summer the Thames, full of sewage, smells so bad that Parliament finally pays for a sewer system for London.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1854\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1854 · Broad Street\",\n"
  "  \"body\": \"In London’s Soho, John Snow maps the cholera deaths and traces them to a single street pump on Broad Street. The handle is removed. It is a founding moment of epidemiology.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1847\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1847 · Semmelweis\",\n"
  "  \"body\": \"In Vienna, Ignaz Semmelweis has doctors wash their hands in chlorinated water. Deaths among new mothers drop sharply.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1842\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1842 · Croton water\",\n"
  "  \"body\": \"On 14 October, New York celebrates the Croton Aqueduct, which has been bringing fresh water into the city’s reservoirs since June.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1837\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1837 · Ice Age\",\n"
  "  \"body\": \"Louis Agassiz tells his colleagues in Neuchâtel that huge sheets of ice once covered much of Europe: the idea of the Ice Age is born.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1966\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1966 · Mud angels\",\n"
  "  \"body\": \"On 4 November the Arno floods Florence. Volunteers from all over the world, the “mud angels”, dig books and paintings out of the silt.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1963\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1963 · Vajont\",\n"
  "  \"body\": \"A mountainside collapses into the Vajont reservoir in Italy. The wave overtops the dam and wipes out the town of Longarone. The dam itself stands to this day.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1962\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1962 · Hamburg surge\",\n"
  "  \"body\": \"In the night of 16 to 17 February a storm surge breaks the dikes of Hamburg. 315 people die in the city.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1954\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1954 · Lucky Dragon\",\n"
  "  \"body\": \"On 1 March the American Castle Bravo hydrogen bomb test at Bikini rains radioactive ash onto a Japanese tuna boat, the Lucky Dragon No. 5. Its crew falls ill.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1953\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1953 · Dutch dikes\",\n"
  "  \"body\": \"In the night of 31 January a North Sea storm flood breaks through the Dutch dikes. More than 1,800 people die. The Netherlands answer with the Delta Works.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1943\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1943 · Broken dams\",\n"
  "  \"body\": \"In the night of 16 to 17 May, British bombers breach the Möhne and Eder dams. The flood wave kills more than 1,600 people, many of them forced labourers.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1935\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1935 · Black Sunday\",\n"
  "  \"body\": \"On 14 April, Black Sunday, a giant dust storm rolls across the drought-stricken American plains. It becomes the symbol of the Dust Bowl.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1917\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1917 · Halifax\",\n"
  "  \"body\": \"On 6 December a munitions ship explodes in Halifax harbour after a collision. The blast raises a tsunami in the harbour and flattens part of the city.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1916\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1916 · Zuiderzee flood\",\n"
  "  \"body\": \"In the night of 13 to 14 January a storm flood breaks the dikes around the Zuiderzee. It convinces the Dutch to close off the sea for good.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1910\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1910 · Paris floods\",\n"
  "  \"body\": \"In January the Seine reaches a level of more than eight metres in Paris. For weeks, Parisians move through their city by boat and on wooden walkways.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1900\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1900 · Galveston\",\n"
  "  \"body\": \"On 8 September a hurricane drives a storm surge over Galveston, Texas. Between 6,000 and 12,000 people die, still the deadliest natural disaster in US history.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1889\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1889 · Johnstown\",\n"
  "  \"body\": \"On 31 May the South Fork Dam gives way above Johnstown, Pennsylvania. The flood kills more than 2,200 people in the valley below.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1883\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1883 · Krakatoa\",\n"
  "  \"body\": \"On 27 August Krakatoa explodes. Its tsunamis kill more than 36,000 people on the coasts of Java and Sumatra.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1868\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1868 · Wateree\",\n"
  "  \"body\": \"On 13 August an earthquake off Arica, then part of Peru, raises a tsunami that carries the American warship Wateree far inland, where it comes to rest upright.\\nSource: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1968\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1968 · Deep drilling\",\n"
  "  \"question\": \"What did the Glomar Challenger’s cores from the ocean floor soon confirm?\",\n"
  "  \"answer\": \"That the seafloor is spreading apart.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1960\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1960 · Challenger Deep\",\n"
  "  \"question\": \"Who reached the bottom of the Challenger Deep in 1960?\",\n"
  "  \"answer\": \"Jacques Piccard and Don Walsh, in the bathyscaphe Trieste, almost 11,000 metres down.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1959\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1959 · Antarctica\",\n"
  "  \"question\": \"About how much of the world’s fresh water is held in Antarctic ice?\",\n"
  "  \"answer\": \"About 70 percent.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1948\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1948 · Salt by air\",\n"
  "  \"question\": \"During the Berlin Airlift, what did the British flying boats carry, and why them?\",\n"
  "  \"answer\": \"Salt. Their hulls were built to resist salt water.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1934\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1934 · Bathysphere\",\n"
  "  \"question\": \"How deep did Beebe and Barton descend in 1934?\",\n"
  "  \"answer\": \"923 metres, in their steel bathysphere off Bermuda.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1920\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1920 · Greater Berlin\",\n"
  "  \"question\": \"Which waters did Berlin gain overnight on 1 October 1920?\",\n"
  "  \"answer\": \"The Müggelsee, the Tegeler See and the Havel lakes, when Greater Berlin was formed.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1896\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1896 · Fram\",\n"
  "  \"question\": \"What did the Fram’s three-year drift in the Arctic ice prove?\",\n"
  "  \"answer\": \"That a current crosses the polar sea.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1894\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1894 · Street pumps\",\n"
  "  \"question\": \"Who designed the cast-iron street pumps Berlin began putting up in 1894?\",\n"
  "  \"answer\": \"Otto Stahn. The pumps were cast in Lauchhammer.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1892\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1892 · Hamburg cholera\",\n"
  "  \"question\": \"Why was Altona largely spared the Hamburg cholera of 1892?\",\n"
  "  \"answer\": \"It filtered its water. Hamburg drank unfiltered water from the Elbe.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1884\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1884 · Cholera germ\",\n"
  "  \"question\": \"Which Berlin doctor identified the cholera bacterium, and where?\",\n"
  "  \"answer\": \"Robert Koch, working in Calcutta in 1884.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1856\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1856 · Waterworks\",\n"
  "  \"question\": \"Where did Berlin’s first waterworks open in 1856?\",\n"
  "  \"answer\": \"At the Stralauer Tor. It was built by an English company.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1854\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1854 · Broad Street\",\n"
  "  \"question\": \"How did John Snow link cholera to one pump in 1854?\",\n"
  "  \"answer\": \"He mapped the deaths in Soho. They led to the Broad Street pump, and its handle was removed.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-qr-quellcode\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Quellcode\",\n"
  "  \"text\": \"Quellcode maps Berlin’s 1968 street pumps. Every pump checked on site opens one more year of water history.\",\n"
  "  \"url\": \"https://quellcode-berlin.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-qr-full\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Full W@terfacts!\",\n"
  "  \"text\": \"The full W@terfacts! grows on the E!nkster Editions station, one fact for every year opened in Quellcode.\",\n"
  "  \"url\": \"https://einkster-editions.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"W@terfacts! Lite\",\n"
  "  \"body\": \"Written by Julien and Claude the AI\"\n"
  " }\n"
  "]\n";
const char WFD_EDITION[] =
  "{\n"
  " \"name\": \"W@terfacts! Lite\",\n"
  " \"slug\": \"waterfacts-lite-de\",\n"
  " \"version\": \"1.0.2\",\n"
  " \"lang\": \"de\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"sources\": [\n"
  "  \"Wikipedia (EN), “Deep Sea Drilling Project”. https://en.wikipedia.org/wiki/Deep_Sea_Drilling_Project\",\n"
  "  \"Wikipedia (EN), “1966 flood of the Arno”. https://en.wikipedia.org/wiki/1966_flood_of_the_Arno\",\n"
  "  \"Wikipedia (DE), “Wassersicherstellungsgesetz”. https://de.wikipedia.org/wiki/Wassersicherstellungsgesetz\",\n"
  "  \"Wikipedia (EN), “Vajont Dam”. https://en.wikipedia.org/wiki/Vajont_Dam\",\n"
  "  \"Wikipedia (EN), “North Sea flood of 1962”. https://en.wikipedia.org/wiki/North_Sea_flood_of_1962\",\n"
  "  \"Wikipedia (EN), “Günter Litfin”. https://en.wikipedia.org/wiki/G%C3%BCnter_Litfin\",\n"
  "  \"Wikipedia (EN), “Trieste (bathyscaphe)”. https://en.wikipedia.org/wiki/Trieste_(bathyscaphe)\",\n"
  "  \"Wikipedia (EN), “Antarctic Treaty System”. https://en.wikipedia.org/wiki/Antarctic_Treaty_System\",\n"
  "  \"Wikipedia (EN), “USS Nautilus (SSN-571)”. https://en.wikipedia.org/wiki/USS_Nautilus_(SSN-571)\",\n"
  "  \"Wikipedia (EN), “The Silent World”. https://en.wikipedia.org/wiki/The_Silent_World\",\n"
  "  \"Wikipedia (EN), “Daigo Fukuryu Maru”. https://en.wikipedia.org/wiki/Daigo_Fukury%C5%AB_Maru\",\n"
  "  \"Wikipedia (EN), “North Sea flood of 1953”. https://en.wikipedia.org/wiki/North_Sea_flood_of_1953\",\n"
  "  \"Wikipedia (EN), “Alain Bombard”. https://en.wikipedia.org/wiki/Alain_Bombard\",\n"
  "  \"Wikipedia (EN), “RV Calypso”. https://en.wikipedia.org/wiki/RV_Calypso\",\n"
  "  \"Wikipedia (EN), “Short Sunderland”. https://en.wikipedia.org/wiki/Short_Sunderland\",\n"
  "  \"Wikipedia (EN), “Kon-Tiki expedition”. https://en.wikipedia.org/wiki/Kon-Tiki_expedition\",\n"
  "  \"Wikipedia (DE), “Berliner Nordsüd-S-Bahn”. https://de.wikipedia.org/wiki/Berliner_Nords%C3%BCd-S-Bahn\",\n"
  "  \"Wikipedia (EN), “Operation Chastise”. https://en.wikipedia.org/wiki/Operation_Chastise\",\n"
  "  \"Wikipedia (EN), “Black Sunday (storm)”. https://en.wikipedia.org/wiki/Black_Sunday_(storm)\",\n"
  "  \"Wikipedia (EN), “Bathysphere”. https://en.wikipedia.org/wiki/Bathysphere\",\n"
  "  \"Wikipedia (EN), “Afsluitdijk”. https://en.wikipedia.org/wiki/Afsluitdijk\",\n"
  "  \"Wikipedia (EN), “Salt March”. https://en.wikipedia.org/wiki/Salt_March\",\n"
  "  \"Wikipedia (EN), “1929 Grand Banks earthquake”. https://en.wikipedia.org/wiki/1929_Grand_Banks_earthquake\",\n"
  "  \"Wikipedia (EN), “Gertrude Ederle”. https://en.wikipedia.org/wiki/Gertrude_Ederle\",\n"
  "  \"Wikipedia (EN), “German Atlantic Expedition”. https://en.wikipedia.org/wiki/German_Atlantic_Expedition\",\n"
  "  \"Wikipedia (EN), “International Hydrographic Organization”. https://en.wikipedia.org/wiki/International_Hydrographic_Organization\",\n"
  "  \"Wikipedia (EN), “Greater Berlin Act”. https://en.wikipedia.org/wiki/Greater_Berlin_Act\",\n"
  "  \"Wikipedia (EN), “Halifax Explosion”. https://en.wikipedia.org/wiki/Halifax_Explosion\",\n"
  "  \"Wikipedia (EN), “Zuiderzee flood of 1916”. https://en.wikipedia.org/wiki/Zuiderzee_flood_of_1916\",\n"
  "  \"Wikipedia (EN), “Panama Canal”. https://en.wikipedia.org/wiki/Panama_Canal\",\n"
  "  \"Wikipedia (EN), “Titanic”. https://en.wikipedia.org/wiki/Titanic\",\n"
  "  \"Wikipedia (EN), “1910 Great Flood of Paris”. https://en.wikipedia.org/wiki/1910_Great_Flood_of_Paris\",\n"
  "  \"Wikipedia (EN), “Teltow Canal”. https://en.wikipedia.org/wiki/Teltow_Canal\",\n"
  "  \"Wikipedia (EN), “Salton Sea”. https://en.wikipedia.org/wiki/Salton_Sea\",\n"
  "  \"Wikipedia (EN), “1900 Galveston hurricane”. https://en.wikipedia.org/wiki/1900_Galveston_hurricane\",\n"
  "  \"Wikipedia (EN), “Belgian Antarctic Expedition”. https://en.wikipedia.org/wiki/Belgian_Antarctic_Expedition\",\n"
  "  \"Wikipedia (EN), “Fram”. https://en.wikipedia.org/wiki/Fram\",\n"
  "  \"Wikipedia (DE), “Straßenbrunnen in Berlin”. https://de.wikipedia.org/wiki/Stra%C3%9Fenbrunnen_in_Berlin\",\n"
  "  \"Wikipedia (EN), “1892 Hamburg cholera epidemic”. https://en.wikipedia.org/wiki/1892_Hamburg_cholera_epidemic\",\n"
  "  \"Wikipedia (EN), “Johnstown Flood”. https://en.wikipedia.org/wiki/Johnstown_Flood\",\n"
  "  \"Wikipedia (EN), “Robert Koch”. https://en.wikipedia.org/wiki/Robert_Koch\",\n"
  "  \"Wikipedia (EN), “1883 eruption of Krakatoa”. https://en.wikipedia.org/wiki/1883_eruption_of_Krakatoa\",\n"
  "  \"Wikipedia (EN), “Challenger expedition”. https://en.wikipedia.org/wiki/Challenger_expedition\",\n"
  "  \"Wikipedia (DE), “James Hobrecht”. https://de.wikipedia.org/wiki/James_Hobrecht\",\n"
  "  \"Wikipedia (EN), “Twenty Thousand Leagues Under the Seas”. https://en.wikipedia.org/wiki/Twenty_Thousand_Leagues_Under_the_Seas\",\n"
  "  \"Wikipedia (EN), “Suez Canal”. https://en.wikipedia.org/wiki/Suez_Canal\",\n"
  "  \"Wikipedia (EN), “1868 Arica earthquake”. https://en.wikipedia.org/wiki/1868_Arica_earthquake\",\n"
  "  \"Wikipedia (EN), “The Water-Babies, A Fairy Tale for a Land Baby”. https://en.wikipedia.org/wiki/The_Water-Babies,_A_Fairy_Tale_for_a_Land_Baby\",\n"
  "  \"Wikipedia (EN), “Robert FitzRoy”. https://en.wikipedia.org/wiki/Robert_FitzRoy\",\n"
  "  \"Wikipedia (EN), “Great Stink”. https://en.wikipedia.org/wiki/Great_Stink\",\n"
  "  \"Wikipedia (EN), “Berliner Wasserbetriebe”. https://en.wikipedia.org/wiki/Berliner_Wasserbetriebe\",\n"
  "  \"Wikipedia (EN), “Matthew Fontaine Maury”. https://en.wikipedia.org/wiki/Matthew_Fontaine_Maury\",\n"
  "  \"Wikipedia (EN), “1854 Broad Street cholera outbreak”. https://en.wikipedia.org/wiki/1854_Broad_Street_cholera_outbreak\",\n"
  "  \"Wikipedia (EN), “Moby-Dick”. https://en.wikipedia.org/wiki/Moby-Dick\",\n"
  "  \"Wikipedia (EN), “Ignaz Semmelweis”. https://en.wikipedia.org/wiki/Ignaz_Semmelweis\",\n"
  "  \"Wikipedia (EN), “Croton Aqueduct”. https://en.wikipedia.org/wiki/Croton_Aqueduct\",\n"
  "  \"Wikipedia (EN), “Louis Agassiz”. https://en.wikipedia.org/wiki/Louis_Agassiz\",\n"
  "  \"Wikipedia (EN), “Second voyage of HMS Beagle”. https://en.wikipedia.org/wiki/Second_voyage_of_HMS_Beagle\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"berlin\",\n"
  "   \"title\": \"Berlin\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"ocean\",\n"
  "   \"title\": \"Ozean\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"world\",\n"
  "   \"title\": \"Welt\",\n"
  "   \"order\": 3\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"chronicle\",\n"
  "   \"title\": \"Chronik\",\n"
  "   \"order\": 4\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"test\",\n"
  "   \"title\": \"Selbsttest\",\n"
  "   \"order\": 5\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"more\",\n"
  "   \"title\": \"Mehr\",\n"
  "   \"order\": 6\n"
  "  }\n"
  " ]\n"
  "}\n";
const char WFD_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"wf-1965\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1965 · Notbrunnen\",\n"
  "  \"body\": \"Am 24. August wird in der Bundesrepublik das Wassersicherstellungsgesetz erlassen. Es ist die Grundlage für die Bundesnotbrunnen, die bis heute in Berlins Straßen stehen.\\nQuelle: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1961\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1961 · Humboldthafen\",\n"
  "  \"body\": \"Am 24. August versucht Günter Litfin, durch den Humboldthafen nach West-Berlin zu schwimmen. Er wird im Wasser erschossen, als erster Mensch, der an der Berliner Mauer erschossen wurde.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1948\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1948 · Salz per Flug\",\n"
  "  \"body\": \"Während der Luftbrücke landen britische Flugboote auf der Havel bei Gatow. Ihre Rümpfe sind gegen Salzwasser geschützt, also bringen sie das Salz für die Stadt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1945\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1945 · Tunnel geflutet\",\n"
  "  \"body\": \"In den ersten Maitagen wird die Decke des S-Bahn-Tunnels unter dem Landwehrkanal gesprengt. Das Kanalwasser flutet den Nord-Süd-Tunnel und große Teile der U-Bahn. Wer den Befehl gab, ist bis heute umstritten.\\nQuelle: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1920\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1920 · Groß-Berlin\",\n"
  "  \"body\": \"Am 1. Oktober entsteht Groß-Berlin. Über Nacht gehören Müggelsee, Tegeler See und die Havelseen zur Stadt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1906\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1906 · Teltowkanal\",\n"
  "  \"body\": \"Am 2. Juni wird der Teltowkanal eröffnet. Er verbindet im Süden Berlins die Havel mit dem Flusssystem der Spree.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1894\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1894 · Straßenpumpen\",\n"
  "  \"body\": \"In Berlins Straßen werden die ersten neuen gusseisernen Pumpen aufgestellt, entworfen von Otto Stahn und gegossen in Lauchhammer. Bis 1897 stehen rund 320 davon.\\nQuelle: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1890\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1890 · Pumpen-Entwurf\",\n"
  "  \"body\": \"Berlin schreibt einen Wettbewerb für eine neue Straßenpumpe aus. Der Siegerentwurf von Otto Stahn wird bald auf Hunderten Gehwegen stehen.\\nQuelle: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1873\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1873 · Hobrecht\",\n"
  "  \"body\": \"Berlin beginnt mit dem Bau von James Hobrechts Radialsystem. Die Kanalisation leitet die Abwässer der Stadt auf Rieselfelder weit vor den Toren.\\nQuelle: Wikipedia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1856\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1856 · Wasserwerk\",\n"
  "  \"body\": \"Am Stralauer Tor geht Berlins erstes Wasserwerk in Betrieb, gebaut von einer englischen Gesellschaft. Nun fließt Wasser durch Rohre in die ersten Häuser.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1968\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1968 · Tiefseebohrung\",\n"
  "  \"body\": \"Das Bohrschiff Glomar Challenger beginnt das Deep Sea Drilling Project. Seine Bohrkerne vom Meeresgrund bestätigen bald, dass der Ozeanboden auseinanderdriftet.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1960\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1960 · Challengertief\",\n"
  "  \"body\": \"Am 23. Januar erreicht das Tauchboot Trieste mit Jacques Piccard und Don Walsh den Grund des Challengertiefs, fast 11.000 Meter unter dem Meeresspiegel.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1958\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1958 · Unter dem Pol\",\n"
  "  \"body\": \"Am 3. August unterquert das U-Boot USS Nautilus als erstes Schiff den Nordpol, unter dem Eis der Arktis hindurch.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1956\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1956 · Welt ohne Sonne\",\n"
  "  \"body\": \"Die Welt des Schweigens, unter Wasser gedreht von Jacques-Yves Cousteau und Louis Malle, gewinnt in Cannes die Goldene Palme.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1952\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1952 · Bombard\",\n"
  "  \"body\": \"Alain Bombard überquert allein in einem Schlauchboot den Atlantik. Er lebt von Fisch und kleinen Mengen Meerwasser, um zu zeigen, dass Schiffbrüchige auf See überleben können.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1950\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1950 · Calypso\",\n"
  "  \"body\": \"Das ehemalige Minensuchboot Calypso wird gekauft und für einen symbolischen Franc im Jahr an Jacques-Yves Cousteau verpachtet. Es wird eines der berühmtesten Forschungsschiffe der Welt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1947\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1947 · Kon-Tiki\",\n"
  "  \"body\": \"Thor Heyerdahl und fünf Gefährten treiben 101 Tage auf dem Balsafloß Kon-Tiki von Peru bis zu einem Riff in Polynesien.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1934\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1934 · Bathysphäre\",\n"
  "  \"body\": \"Am 15. August tauchen William Beebe und Otis Barton in ihrer Stahlkugel vor Bermuda auf 923 Meter hinab, tiefer als je ein Mensch zuvor.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1929\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1929 · Kabelbruch\",\n"
  "  \"body\": \"Am 18. November löst ein Beben vor Neufundland einen Unterwasser-Erdrutsch aus. Er reißt nacheinander transatlantische Telegrafenkabel, und ein Tsunami trifft die Küste.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1925\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1925 · Meteor\",\n"
  "  \"body\": \"Das deutsche Forschungsschiff Meteor bricht zur Vermessung des Südatlantiks auf. Mit einem Echolot kartiert es den Meeresboden wie nie zuvor.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1921\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1921 · Seekarten\",\n"
  "  \"body\": \"In Monaco wird das Internationale Hydrographische Büro gegründet, damit die Seekarten der Welt endlich dieselbe Sprache sprechen.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1912\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1912 · Titanic\",\n"
  "  \"body\": \"In der Nacht vom 14. auf den 15. April rammt die Titanic einen Eisberg und sinkt im Nordatlantik. Mehr als 1.500 Menschen sterben im fast gefrierenden Wasser.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1897\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1897 · Belgica\",\n"
  "  \"body\": \"Die Belgica bricht in die Antarktis auf. Im Packeis eingeschlossen, überwintert ihre Besatzung als erste in der Antarktis.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1896\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1896 · Fram\",\n"
  "  \"body\": \"Nansens Schiff Fram kehrt nach Norwegen zurück, nachdem es drei Jahre im Eis der Arktis festgefroren trieb. Die Drift beweist, dass eine Strömung das Polarmeer durchquert.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1875\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1875 · Tiefste Lotung\",\n"
  "  \"body\": \"Die HMS Challenger lotet im Marianengraben mehr als 8.000 Meter aus, die größte damals bekannte Meerestiefe.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1872\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1872 · Challenger\",\n"
  "  \"body\": \"Am 21. Dezember verlässt die HMS Challenger Portsmouth: die erste große Reise, die ganz der Erforschung der Meere gewidmet ist.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1870\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1870 · Nemo\",\n"
  "  \"body\": \"Jules Vernes Zwanzigtausend Meilen unter dem Meer erscheint als Buch. Kapitän Nemos Nautilus schickt Generationen von Lesern unter Wasser.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1855\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1855 · Maury\",\n"
  "  \"body\": \"Matthew Fontaine Maury veröffentlicht Die physische Geographie des Meeres, oft das erste Lehrbuch der modernen Ozeanographie genannt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1851\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1851 · Moby-Dick\",\n"
  "  \"body\": \"Herman Melville veröffentlicht Moby-Dick, die Geschichte von Kapitän Ahabs Jagd auf einen weißen Wal.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1835\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1835 · Galápagos\",\n"
  "  \"body\": \"Die Beagle erreicht die Galápagosinseln. Darwin fällt auf, dass jede Insel ihre eigenen Spottdrosseln und Riesenschildkröten zu haben scheint.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1959\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1959 · Antarktis\",\n"
  "  \"body\": \"Am 1. Dezember unterzeichnen zwölf Staaten den Antarktisvertrag: Der Kontinent bleibt Frieden und Forschung vorbehalten. Sein Eis speichert rund 70 Prozent des Süßwassers der Erde.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1932\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1932 · Abschlussdeich\",\n"
  "  \"body\": \"Am 28. Mai schließen die Niederländer den Abschlussdeich. Aus der salzigen Zuiderzee wird ein Süßwassersee, das IJsselmeer.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1930\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1930 · Salzmarsch\",\n"
  "  \"body\": \"Gandhi läuft 24 Tage bis ans Meer bei Dandi und gewinnt Salz aus Meerwasser, gegen das britische Salzmonopol.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1926\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1926 · Kanalschwimmen\",\n"
  "  \"body\": \"Am 6. August durchschwimmt Gertrude Ederle als erste Frau den Ärmelkanal und unterbietet den Rekord der Männer um fast zwei Stunden.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1914\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1914 · Panamakanal\",\n"
  "  \"body\": \"Am 15. August wird der Panamakanal eröffnet. Schiffe müssen nicht mehr um Südamerika herumfahren, um vom Atlantik in den Pazifik zu gelangen.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1905\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1905 · Saltonsee\",\n"
  "  \"body\": \"Der Colorado bricht durch einen Bewässerungskanal und strömt fast zwei Jahre lang in ein Wüstenbecken Kaliforniens. So entsteht der Salton Sea.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1892\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1892 · Cholera\",\n"
  "  \"body\": \"Die Cholera tötet in Hamburg rund 8.600 Menschen. Die Stadt trinkt ungefiltertes Elbwasser. Das benachbarte Altona filtert sein Wasser und bleibt weitgehend verschont.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1884\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1884 · Choleraerreger\",\n"
  "  \"body\": \"Der Berliner Arzt Robert Koch identifiziert in Kalkutta den Cholera-Erreger und zeigt, dass er sich über verunreinigtes Wasser verbreitet.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1869\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1869 · Suezkanal\",\n"
  "  \"body\": \"Am 17. November wird der Suezkanal eröffnet und verbindet das Mittelmeer mit dem Roten Meer.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1863\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1863 · Wasserkinder\",\n"
  "  \"body\": \"Charles Kingsleys Die Wasserkinder erscheint als Buch: Ein kleiner Schornsteinfeger fällt in einen Fluss und wird zu einem Wesen aus Fluss und Meer.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1861\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1861 · Wetterbericht\",\n"
  "  \"body\": \"Am 1. August druckt die Times die ersten öffentlichen Wettervorhersagen, erstellt von Robert FitzRoy, dem früheren Kapitän der Beagle.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1858\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1858 · Großer Gestank\",\n"
  "  \"body\": \"In einem heißen Sommer stinkt die mit Abwasser gefüllte Themse so sehr, dass das Parlament endlich eine Kanalisation für London bewilligt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1854\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1854 · Broad Street\",\n"
  "  \"body\": \"Im Londoner Stadtteil Soho kartiert John Snow die Cholera-Toten und führt sie auf eine einzige Straßenpumpe in der Broad Street zurück. Der Schwengel wird abmontiert. Es ist eine Geburtsstunde der Epidemiologie.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1847\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1847 · Semmelweis\",\n"
  "  \"body\": \"In Wien lässt Ignaz Semmelweis die Ärzte ihre Hände in Chlorwasser waschen. Die Todesfälle unter jungen Müttern gehen drastisch zurück.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1842\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1842 · Croton-Aquädukt\",\n"
  "  \"body\": \"Am 14. Oktober feiert New York den Croton-Aquädukt, der seit Juni frisches Wasser in die Reservoirs der Stadt leitet.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1837\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1837 · Eiszeit\",\n"
  "  \"body\": \"Louis Agassiz erklärt seinen Kollegen in Neuenburg, dass einst riesige Eismassen große Teile Europas bedeckten: Die Idee der Eiszeit ist geboren.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1966\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1966 · Schlammengel\",\n"
  "  \"body\": \"Am 4. November überflutet der Arno Florenz. Freiwillige aus aller Welt, die „Schlammengel“, graben Bücher und Gemälde aus dem Schlamm.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1963\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1963 · Vajont\",\n"
  "  \"body\": \"Am Vajont in Italien stürzt ein Berghang in den Stausee. Die Flutwelle schwappt über die Mauer und zerstört Longarone. Die Staumauer selbst steht bis heute.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1962\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1962 · Hamburger Flut\",\n"
  "  \"body\": \"In der Nacht vom 16. auf den 17. Februar bricht eine Sturmflut die Deiche Hamburgs. In der Stadt sterben 315 Menschen.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1954\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1954 · Bikini-Asche\",\n"
  "  \"body\": \"Am 1. März lässt der amerikanische Wasserstoffbombentest Castle Bravo am Bikini-Atoll radioaktive Asche auf den japanischen Thunfischkutter Glücklicher Drache Nr. 5 regnen. Die Besatzung erkrankt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1953\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1953 · Hollands Deiche\",\n"
  "  \"body\": \"In der Nacht zum 1. Februar bricht eine Sturmflut der Nordsee durch die niederländischen Deiche. Mehr als 1.800 Menschen sterben. Die Niederlande antworten mit den Deltawerken.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1943\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1943 · Dammbruch\",\n"
  "  \"body\": \"In der Nacht vom 16. auf den 17. Mai zerstören britische Bomber die Möhne- und die Edertalsperre. Die Flutwelle tötet mehr als 1.600 Menschen, viele davon Zwangsarbeiter.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1935\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1935 · Staubsturm\",\n"
  "  \"body\": \"Am 14. April, dem Schwarzen Sonntag, rollt ein riesiger Staubsturm über die ausgedörrten Prärien Amerikas. Er wird zum Sinnbild der Dust Bowl.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1917\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1917 · Halifax\",\n"
  "  \"body\": \"Am 6. Dezember explodiert nach einer Kollision ein Munitionsschiff im Hafen von Halifax. Die Druckwelle löst eine Flutwelle im Hafen aus und zerstört einen Teil der Stadt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1916\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1916 · Zuiderzee-Flut\",\n"
  "  \"body\": \"In der Nacht vom 13. auf den 14. Januar bricht eine Sturmflut die Deiche an der Zuiderzee. Sie überzeugt die Niederländer, das Meer für immer abzuriegeln.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1910\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1910 · Paris im Wasser\",\n"
  "  \"body\": \"Im Januar erreicht die Seine in Paris einen Pegel von über acht Metern. Wochenlang bewegen sich die Pariser mit Booten und über Holzstege durch ihre Stadt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1900\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1900 · Galveston\",\n"
  "  \"body\": \"Am 8. September drückt ein Hurrikan eine Sturmflut über Galveston in Texas. Zwischen 6.000 und 12.000 Menschen sterben, bis heute die tödlichste Naturkatastrophe der USA.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1889\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1889 · Johnstown\",\n"
  "  \"body\": \"Am 31. Mai bricht oberhalb von Johnstown in Pennsylvania der South-Fork-Damm. Die Flut tötet im Tal darunter mehr als 2.200 Menschen.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1883\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1883 · Krakatau\",\n"
  "  \"body\": \"Am 27. August explodiert der Krakatau. Seine Tsunamis töten an den Küsten von Java und Sumatra mehr als 36.000 Menschen.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1868\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1868 · Wateree\",\n"
  "  \"body\": \"Am 13. August löst ein Beben vor Arica, damals Peru, einen Tsunami aus. Er trägt das amerikanische Kriegsschiff Wateree weit ins Landesinnere, wo es aufrecht liegen bleibt.\\nQuelle: Wikipedia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1968\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1968 · Tiefseebohrung\",\n"
  "  \"question\": \"Was bestätigten die Bohrkerne der Glomar Challenger bald?\",\n"
  "  \"answer\": \"Dass der Ozeanboden auseinanderdriftet.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1960\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1960 · Challengertief\",\n"
  "  \"question\": \"Wer erreichte 1960 den Grund des Challengertiefs?\",\n"
  "  \"answer\": \"Jacques Piccard und Don Walsh, im Tauchboot Trieste, fast 11.000 Meter tief.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1959\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1959 · Antarktis\",\n"
  "  \"question\": \"Wie viel des Süßwassers der Welt steckt ungefähr im Eis der Antarktis?\",\n"
  "  \"answer\": \"Etwa 70 Prozent.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1948\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1948 · Salz per Flug\",\n"
  "  \"question\": \"Was transportierten die britischen Flugboote während der Luftbrücke, und warum gerade sie?\",\n"
  "  \"answer\": \"Salz. Ihre Rümpfe waren gegen Salzwasser gebaut.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1934\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1934 · Bathysphäre\",\n"
  "  \"question\": \"Wie tief tauchten Beebe und Barton 1934?\",\n"
  "  \"answer\": \"923 Meter, in ihrer stählernen Bathysphäre vor den Bermudas.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1920\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1920 · Groß-Berlin\",\n"
  "  \"question\": \"Welche Gewässer gewann Berlin am 1. Oktober 1920 über Nacht dazu?\",\n"
  "  \"answer\": \"Den Müggelsee, den Tegeler See und die Havelseen, mit der Bildung von Groß-Berlin.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1896\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1896 · Fram\",\n"
  "  \"question\": \"Was bewies die dreijährige Drift der Fram im arktischen Eis?\",\n"
  "  \"answer\": \"Dass eine Strömung das Polarmeer durchquert.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1894\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1894 · Straßenpumpen\",\n"
  "  \"question\": \"Wer entwarf die gusseisernen Straßenpumpen, die Berlin ab 1894 aufstellte?\",\n"
  "  \"answer\": \"Otto Stahn. Gegossen wurden die Pumpen in Lauchhammer.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1892\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1892 · Cholera\",\n"
  "  \"question\": \"Warum blieb Altona von der Hamburger Cholera 1892 weitgehend verschont?\",\n"
  "  \"answer\": \"Es filterte sein Wasser. Hamburg trank ungefiltertes Elbwasser.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1884\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1884 · Choleraerreger\",\n"
  "  \"question\": \"Welcher Berliner Arzt identifizierte den Choleraerreger, und wo?\",\n"
  "  \"answer\": \"Robert Koch, 1884 in Kalkutta.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1856\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1856 · Wasserwerk\",\n"
  "  \"question\": \"Wo ging 1856 Berlins erstes Wasserwerk in Betrieb?\",\n"
  "  \"answer\": \"Am Stralauer Tor. Gebaut hat es eine englische Gesellschaft.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1854\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1854 · Broad Street\",\n"
  "  \"question\": \"Wie brachte John Snow 1854 die Cholera mit einer einzigen Pumpe in Verbindung?\",\n"
  "  \"answer\": \"Er kartierte die Todesfälle in Soho. Sie führten zur Pumpe in der Broad Street, deren Schwengel entfernt wurde.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-qr-quellcode\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Quellcode\",\n"
  "  \"text\": \"Quellcode kartiert Berlins 1968 Straßenpumpen. Jede vor Ort geprüfte Pumpe öffnet ein weiteres Jahr Wassergeschichte.\",\n"
  "  \"url\": \"https://quellcode-berlin.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-qr-full\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Ganz W@terfacts!\",\n"
  "  \"text\": \"Das ganze W@terfacts! wächst auf der E!nkster-Editions-Station: ein Fakt für jedes in Quellcode geöffnete Jahr.\",\n"
  "  \"url\": \"https://einkster-editions.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"W@terfacts! Lite\",\n"
  "  \"body\": \"Geschrieben von Julien und Claude, der KI\"\n"
  " }\n"
  "]\n";
const char WFF_EDITION[] =
  "{\n"
  " \"name\": \"W@terfacts! Lite\",\n"
  " \"slug\": \"waterfacts-lite-fr\",\n"
  " \"version\": \"1.0.2\",\n"
  " \"lang\": \"fr\",\n"
  " \"authors\": [\n"
  "  \"InvisibleChaos\"\n"
  " ],\n"
  " \"license\": \"CC BY 4.0\",\n"
  " \"sources\": [\n"
  "  \"Wikipedia (EN), “Deep Sea Drilling Project”. https://en.wikipedia.org/wiki/Deep_Sea_Drilling_Project\",\n"
  "  \"Wikipedia (EN), “1966 flood of the Arno”. https://en.wikipedia.org/wiki/1966_flood_of_the_Arno\",\n"
  "  \"Wikipedia (DE), “Wassersicherstellungsgesetz”. https://de.wikipedia.org/wiki/Wassersicherstellungsgesetz\",\n"
  "  \"Wikipedia (EN), “Vajont Dam”. https://en.wikipedia.org/wiki/Vajont_Dam\",\n"
  "  \"Wikipedia (EN), “North Sea flood of 1962”. https://en.wikipedia.org/wiki/North_Sea_flood_of_1962\",\n"
  "  \"Wikipedia (EN), “Günter Litfin”. https://en.wikipedia.org/wiki/G%C3%BCnter_Litfin\",\n"
  "  \"Wikipedia (EN), “Trieste (bathyscaphe)”. https://en.wikipedia.org/wiki/Trieste_(bathyscaphe)\",\n"
  "  \"Wikipedia (EN), “Antarctic Treaty System”. https://en.wikipedia.org/wiki/Antarctic_Treaty_System\",\n"
  "  \"Wikipedia (EN), “USS Nautilus (SSN-571)”. https://en.wikipedia.org/wiki/USS_Nautilus_(SSN-571)\",\n"
  "  \"Wikipedia (EN), “The Silent World”. https://en.wikipedia.org/wiki/The_Silent_World\",\n"
  "  \"Wikipedia (EN), “Daigo Fukuryu Maru”. https://en.wikipedia.org/wiki/Daigo_Fukury%C5%AB_Maru\",\n"
  "  \"Wikipedia (EN), “North Sea flood of 1953”. https://en.wikipedia.org/wiki/North_Sea_flood_of_1953\",\n"
  "  \"Wikipedia (EN), “Alain Bombard”. https://en.wikipedia.org/wiki/Alain_Bombard\",\n"
  "  \"Wikipedia (EN), “RV Calypso”. https://en.wikipedia.org/wiki/RV_Calypso\",\n"
  "  \"Wikipedia (EN), “Short Sunderland”. https://en.wikipedia.org/wiki/Short_Sunderland\",\n"
  "  \"Wikipedia (EN), “Kon-Tiki expedition”. https://en.wikipedia.org/wiki/Kon-Tiki_expedition\",\n"
  "  \"Wikipedia (DE), “Berliner Nordsüd-S-Bahn”. https://de.wikipedia.org/wiki/Berliner_Nords%C3%BCd-S-Bahn\",\n"
  "  \"Wikipedia (EN), “Operation Chastise”. https://en.wikipedia.org/wiki/Operation_Chastise\",\n"
  "  \"Wikipedia (EN), “Black Sunday (storm)”. https://en.wikipedia.org/wiki/Black_Sunday_(storm)\",\n"
  "  \"Wikipedia (EN), “Bathysphere”. https://en.wikipedia.org/wiki/Bathysphere\",\n"
  "  \"Wikipedia (EN), “Afsluitdijk”. https://en.wikipedia.org/wiki/Afsluitdijk\",\n"
  "  \"Wikipedia (EN), “Salt March”. https://en.wikipedia.org/wiki/Salt_March\",\n"
  "  \"Wikipedia (EN), “1929 Grand Banks earthquake”. https://en.wikipedia.org/wiki/1929_Grand_Banks_earthquake\",\n"
  "  \"Wikipedia (EN), “Gertrude Ederle”. https://en.wikipedia.org/wiki/Gertrude_Ederle\",\n"
  "  \"Wikipedia (EN), “German Atlantic Expedition”. https://en.wikipedia.org/wiki/German_Atlantic_Expedition\",\n"
  "  \"Wikipedia (EN), “International Hydrographic Organization”. https://en.wikipedia.org/wiki/International_Hydrographic_Organization\",\n"
  "  \"Wikipedia (EN), “Greater Berlin Act”. https://en.wikipedia.org/wiki/Greater_Berlin_Act\",\n"
  "  \"Wikipedia (EN), “Halifax Explosion”. https://en.wikipedia.org/wiki/Halifax_Explosion\",\n"
  "  \"Wikipedia (EN), “Zuiderzee flood of 1916”. https://en.wikipedia.org/wiki/Zuiderzee_flood_of_1916\",\n"
  "  \"Wikipedia (EN), “Panama Canal”. https://en.wikipedia.org/wiki/Panama_Canal\",\n"
  "  \"Wikipedia (EN), “Titanic”. https://en.wikipedia.org/wiki/Titanic\",\n"
  "  \"Wikipedia (EN), “1910 Great Flood of Paris”. https://en.wikipedia.org/wiki/1910_Great_Flood_of_Paris\",\n"
  "  \"Wikipedia (EN), “Teltow Canal”. https://en.wikipedia.org/wiki/Teltow_Canal\",\n"
  "  \"Wikipedia (EN), “Salton Sea”. https://en.wikipedia.org/wiki/Salton_Sea\",\n"
  "  \"Wikipedia (EN), “1900 Galveston hurricane”. https://en.wikipedia.org/wiki/1900_Galveston_hurricane\",\n"
  "  \"Wikipedia (EN), “Belgian Antarctic Expedition”. https://en.wikipedia.org/wiki/Belgian_Antarctic_Expedition\",\n"
  "  \"Wikipedia (EN), “Fram”. https://en.wikipedia.org/wiki/Fram\",\n"
  "  \"Wikipedia (DE), “Straßenbrunnen in Berlin”. https://de.wikipedia.org/wiki/Stra%C3%9Fenbrunnen_in_Berlin\",\n"
  "  \"Wikipedia (EN), “1892 Hamburg cholera epidemic”. https://en.wikipedia.org/wiki/1892_Hamburg_cholera_epidemic\",\n"
  "  \"Wikipedia (EN), “Johnstown Flood”. https://en.wikipedia.org/wiki/Johnstown_Flood\",\n"
  "  \"Wikipedia (EN), “Robert Koch”. https://en.wikipedia.org/wiki/Robert_Koch\",\n"
  "  \"Wikipedia (EN), “1883 eruption of Krakatoa”. https://en.wikipedia.org/wiki/1883_eruption_of_Krakatoa\",\n"
  "  \"Wikipedia (EN), “Challenger expedition”. https://en.wikipedia.org/wiki/Challenger_expedition\",\n"
  "  \"Wikipedia (DE), “James Hobrecht”. https://de.wikipedia.org/wiki/James_Hobrecht\",\n"
  "  \"Wikipedia (EN), “Twenty Thousand Leagues Under the Seas”. https://en.wikipedia.org/wiki/Twenty_Thousand_Leagues_Under_the_Seas\",\n"
  "  \"Wikipedia (EN), “Suez Canal”. https://en.wikipedia.org/wiki/Suez_Canal\",\n"
  "  \"Wikipedia (EN), “1868 Arica earthquake”. https://en.wikipedia.org/wiki/1868_Arica_earthquake\",\n"
  "  \"Wikipedia (EN), “The Water-Babies, A Fairy Tale for a Land Baby”. https://en.wikipedia.org/wiki/The_Water-Babies,_A_Fairy_Tale_for_a_Land_Baby\",\n"
  "  \"Wikipedia (EN), “Robert FitzRoy”. https://en.wikipedia.org/wiki/Robert_FitzRoy\",\n"
  "  \"Wikipedia (EN), “Great Stink”. https://en.wikipedia.org/wiki/Great_Stink\",\n"
  "  \"Wikipedia (EN), “Berliner Wasserbetriebe”. https://en.wikipedia.org/wiki/Berliner_Wasserbetriebe\",\n"
  "  \"Wikipedia (EN), “Matthew Fontaine Maury”. https://en.wikipedia.org/wiki/Matthew_Fontaine_Maury\",\n"
  "  \"Wikipedia (EN), “1854 Broad Street cholera outbreak”. https://en.wikipedia.org/wiki/1854_Broad_Street_cholera_outbreak\",\n"
  "  \"Wikipedia (EN), “Moby-Dick”. https://en.wikipedia.org/wiki/Moby-Dick\",\n"
  "  \"Wikipedia (EN), “Ignaz Semmelweis”. https://en.wikipedia.org/wiki/Ignaz_Semmelweis\",\n"
  "  \"Wikipedia (EN), “Croton Aqueduct”. https://en.wikipedia.org/wiki/Croton_Aqueduct\",\n"
  "  \"Wikipedia (EN), “Louis Agassiz”. https://en.wikipedia.org/wiki/Louis_Agassiz\",\n"
  "  \"Wikipedia (EN), “Second voyage of HMS Beagle”. https://en.wikipedia.org/wiki/Second_voyage_of_HMS_Beagle\"\n"
  " ],\n"
  " \"sections\": [\n"
  "  {\n"
  "   \"id\": \"berlin\",\n"
  "   \"title\": \"Berlin\",\n"
  "   \"order\": 1\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"ocean\",\n"
  "   \"title\": \"Océan\",\n"
  "   \"order\": 2\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"world\",\n"
  "   \"title\": \"Monde\",\n"
  "   \"order\": 3\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"chronicle\",\n"
  "   \"title\": \"Chronique\",\n"
  "   \"order\": 4\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"test\",\n"
  "   \"title\": \"Testez-vous\",\n"
  "   \"order\": 5\n"
  "  },\n"
  "  {\n"
  "   \"id\": \"more\",\n"
  "   \"title\": \"Plus\",\n"
  "   \"order\": 6\n"
  "  }\n"
  " ]\n"
  "}\n";
const char WFF_CARDS[] =
  "[\n"
  " {\n"
  "  \"id\": \"wf-1965\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1965 · Puits d’urgence\",\n"
  "  \"body\": \"Le 24 août, l’Allemagne de l’Ouest adopte sa loi sur la sécurité de l’approvisionnement en eau. Elle fonde les puits de secours fédéraux encore visibles dans les rues de Berlin.\\nSource : Wikipédia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1961\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1961 · Humboldthafen\",\n"
  "  \"body\": \"Le 24 août, Günter Litfin tente de rejoindre Berlin-Ouest à la nage par le Humboldthafen. Il est abattu dans l’eau, premier tué par balle au Mur de Berlin.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1948\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1948 · Sel par avion\",\n"
  "  \"body\": \"Pendant le pont aérien de Berlin, des hydravions britanniques se posent sur la Havel près de Gatow. Leurs coques résistent à l’eau salée : ils transportent donc le sel de la ville.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1945\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1945 · Tunnel inondé\",\n"
  "  \"body\": \"Dans les premiers jours de mai, le plafond du tunnel du S-Bahn sous le Landwehrkanal est dynamité. L’eau du canal inonde le tunnel nord-sud et une grande partie du métro. On ignore toujours qui en a donné l’ordre.\\nSource : Wikipédia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1920\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1920 · Grand Berlin\",\n"
  "  \"body\": \"Le 1er octobre naît le Grand Berlin. Du jour au lendemain, la ville gagne le Müggelsee, le Tegeler See et les lacs de la Havel.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1906\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1906 · Canal de Teltow\",\n"
  "  \"body\": \"Le 2 juin, le canal de Teltow est inauguré. Il relie la Havel au réseau de la Spree par le sud de Berlin.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1894\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1894 · Pompes de rue\",\n"
  "  \"body\": \"Les premières nouvelles pompes en fonte sont installées dans les rues de Berlin, dessinées par Otto Stahn et coulées à Lauchhammer. En 1897, il y en a environ 320.\\nSource : Wikipédia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1890\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1890 · Concours\",\n"
  "  \"body\": \"Berlin lance un concours pour une nouvelle pompe de rue. Le projet lauréat d’Otto Stahn se dressera bientôt sur des centaines de trottoirs.\\nSource : Wikipédia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1873\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1873 · Égouts Hobrecht\",\n"
  "  \"body\": \"Berlin commence à construire le réseau radial d’égouts de James Hobrecht, qui évacue les eaux usées vers des champs d’épandage loin hors de la ville.\\nSource : Wikipédia (DE)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1856\",\n"
  "  \"section\": \"berlin\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1856 · Première usine\",\n"
  "  \"body\": \"La première usine des eaux de Berlin ouvre au Stralauer Tor, construite par une compagnie anglaise. L’eau arrive désormais par des tuyaux dans les premières maisons.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1968\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1968 · Forage profond\",\n"
  "  \"body\": \"Le navire de forage Glomar Challenger lance le Deep Sea Drilling Project. Ses carottes prélevées au fond de l’océan confirmeront bientôt l’expansion des fonds océaniques.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1960\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1960 · Challenger Deep\",\n"
  "  \"body\": \"Le 23 janvier, le bathyscaphe Trieste, avec Jacques Piccard et Don Walsh à bord, touche le fond de la fosse Challenger, à près de 11 000 mètres.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1958\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1958 · Sous le pôle\",\n"
  "  \"body\": \"Le 3 août, le sous-marin USS Nautilus devient le premier navire à passer sous le pôle Nord, sous la banquise arctique.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1956\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1956 · Cousteau\",\n"
  "  \"body\": \"Le Monde du silence, tourné sous l’eau par Jacques-Yves Cousteau et Louis Malle, remporte la Palme d’or à Cannes.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1952\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1952 · Bombard\",\n"
  "  \"body\": \"Alain Bombard traverse seul l’Atlantique en canot pneumatique, en vivant de poisson et de petites quantités d’eau de mer, pour prouver que les naufragés peuvent survivre.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1950\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1950 · Calypso\",\n"
  "  \"body\": \"L’ancien dragueur de mines Calypso est acheté et loué à Jacques-Yves Cousteau pour un franc symbolique par an. Il deviendra l’un des navires de recherche les plus célèbres du monde.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1947\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1947 · Kon-Tiki\",\n"
  "  \"body\": \"Thor Heyerdahl et cinq compagnons dérivent 101 jours sur le radeau de balsa Kon-Tiki, du Pérou jusqu’à un récif de Polynésie.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1934\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1934 · Bathysphère\",\n"
  "  \"body\": \"Le 15 août, William Beebe et Otis Barton descendent dans leur bathysphère d’acier à 923 mètres au large des Bermudes, plus profond que quiconque avant eux.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1929\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1929 · Câbles rompus\",\n"
  "  \"body\": \"Le 18 novembre, un séisme au large de Terre-Neuve provoque un glissement sous-marin qui rompt un à un les câbles télégraphiques transatlantiques, et un tsunami frappe la côte.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1925\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1925 · Meteor\",\n"
  "  \"body\": \"Le navire de recherche allemand Meteor part étudier l’Atlantique Sud. Grâce à un sondeur acoustique, il cartographie les fonds marins comme jamais auparavant.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1921\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1921 · Cartes marines\",\n"
  "  \"body\": \"Le Bureau hydrographique international est fondé à Monaco, pour que les cartes marines du monde parlent enfin la même langue.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1912\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1912 · Titanic\",\n"
  "  \"body\": \"Dans la nuit du 14 au 15 avril, le Titanic heurte un iceberg et sombre dans l’Atlantique Nord. Plus de 1 500 personnes meurent dans une eau proche du gel.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1897\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1897 · Belgica\",\n"
  "  \"body\": \"La Belgica part pour l’Antarctique. Prisonnier de la banquise, son équipage devient le premier à hiverner en Antarctique.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1896\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1896 · Fram\",\n"
  "  \"body\": \"Le Fram de Nansen rentre en Norvège après avoir dérivé trois ans, pris dans les glaces arctiques. Sa dérive prouve qu’un courant traverse la mer polaire.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1875\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1875 · Sonde record\",\n"
  "  \"body\": \"Le HMS Challenger sonde plus de 8 000 mètres dans la fosse des Mariannes, la plus grande profondeur océanique connue à l’époque.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1872\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1872 · Challenger\",\n"
  "  \"body\": \"Le 21 décembre, le HMS Challenger quitte Portsmouth pour le premier grand voyage entièrement consacré à l’étude des océans.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1870\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1870 · Nemo\",\n"
  "  \"body\": \"Vingt mille lieues sous les mers, de Jules Verne, paraît en volume. Le Nautilus du capitaine Nemo entraîne des générations de lecteurs sous l’eau.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1855\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1855 · Maury\",\n"
  "  \"body\": \"Matthew Fontaine Maury publie La Géographie physique de la mer, souvent considérée comme le premier manuel d’océanographie moderne.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1851\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1851 · Moby Dick\",\n"
  "  \"body\": \"Herman Melville publie Moby Dick, l’histoire de la chasse du capitaine Achab à une baleine blanche.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1835\",\n"
  "  \"section\": \"ocean\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1835 · Galápagos\",\n"
  "  \"body\": \"Le Beagle atteint les îles Galápagos. Darwin remarque que chaque île semble avoir ses propres moqueurs et ses propres tortues géantes.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1959\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1959 · Antarctique\",\n"
  "  \"body\": \"Le 1er décembre, douze États signent le traité sur l’Antarctique, qui réserve le continent à la paix et à la science. Sa glace contient environ 70 % de l’eau douce du globe.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1932\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1932 · Afsluitdijk\",\n"
  "  \"body\": \"Le 28 mai, les Néerlandais ferment la digue de l’Afsluitdijk. Le Zuiderzee, salé, devient un lac d’eau douce, l’IJsselmeer.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1930\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1930 · Marche du sel\",\n"
  "  \"body\": \"Gandhi marche 24 jours jusqu’à la mer, à Dandi, et tire du sel de l’eau de mer, défiant le monopole britannique.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1926\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1926 · Manche à nage\",\n"
  "  \"body\": \"Le 6 août, Gertrude Ederle devient la première femme à traverser la Manche à la nage, et bat le record masculin de près de deux heures.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1914\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1914 · Canal de Panama\",\n"
  "  \"body\": \"Le 15 août, le canal de Panama ouvre. Les navires n’ont plus besoin de contourner l’Amérique du Sud pour passer de l’Atlantique au Pacifique.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1905\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1905 · Salton Sea\",\n"
  "  \"body\": \"Le Colorado perce un canal d’irrigation et se déverse pendant près de deux ans dans un bassin désertique de Californie. Ainsi naît la Salton Sea.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1892\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1892 · Choléra\",\n"
  "  \"body\": \"Le choléra tue environ 8 600 personnes à Hambourg, qui boit l’eau de l’Elbe sans la filtrer. Altona, la ville voisine, filtre son eau et est presque épargnée.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1884\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1884 · Koch\",\n"
  "  \"body\": \"À Calcutta, le médecin berlinois Robert Koch identifie la bactérie du choléra et montre qu’elle se propage par l’eau contaminée.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1869\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1869 · Canal de Suez\",\n"
  "  \"body\": \"Le 17 novembre, le canal de Suez ouvre et relie la Méditerranée à la mer Rouge.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1863\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1863 · Bébés d’eau\",\n"
  "  \"body\": \"Les Bébés d’eau de Charles Kingsley paraît en volume : un petit ramoneur tombe dans une rivière et devient une créature de la rivière et de la mer.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1861\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1861 · Première météo\",\n"
  "  \"body\": \"Le 1er août, le Times publie les premières prévisions météorologiques publiques, établies par Robert FitzRoy, ancien capitaine du Beagle.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1858\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1858 · Grande Puanteur\",\n"
  "  \"body\": \"Lors d’un été brûlant, la Tamise, pleine d’eaux usées, empeste tant que le Parlement finance enfin un réseau d’égouts pour Londres.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1854\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1854 · Broad Street\",\n"
  "  \"body\": \"Dans le quartier londonien de Soho, John Snow cartographie les morts du choléra et remonte jusqu’à une seule pompe de rue, sur Broad Street. On en retire le levier. C’est l’acte de naissance de l’épidémiologie.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1847\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1847 · Semmelweis\",\n"
  "  \"body\": \"À Vienne, Ignace Semmelweis fait laver les mains des médecins dans de l’eau chlorée. La mortalité des jeunes mères chute brutalement.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1842\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1842 · Eau du Croton\",\n"
  "  \"body\": \"Le 14 octobre, New York fête l’aqueduc du Croton, qui alimente en eau douce les réservoirs de la ville depuis juin.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1837\",\n"
  "  \"section\": \"world\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1837 · Âge de glace\",\n"
  "  \"body\": \"Louis Agassiz explique à ses collègues de Neuchâtel que d’immenses glaciers ont jadis recouvert une grande partie de l’Europe : l’idée de l’ère glaciaire est née.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1966\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1966 · Anges de boue\",\n"
  "  \"body\": \"Le 4 novembre, l’Arno submerge Florence. Des bénévoles venus du monde entier, les « anges de la boue », sortent livres et tableaux de la vase.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1963\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1963 · Vajont\",\n"
  "  \"body\": \"Un pan de montagne s’effondre dans le lac de retenue du Vajont, en Italie. La vague passe par-dessus le barrage et anéantit Longarone. Le barrage, lui, tient toujours.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1962\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1962 · Hambourg noyée\",\n"
  "  \"body\": \"Dans la nuit du 16 au 17 février, une onde de tempête rompt les digues de Hambourg. 315 personnes meurent dans la ville.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1954\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1954 · Dragon chanceux\",\n"
  "  \"body\": \"Le 1er mars, l’essai américain de bombe H Castle Bravo, à Bikini, fait pleuvoir des cendres radioactives sur un thonier japonais, le Dragon chanceux n° 5. L’équipage tombe malade.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1953\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1953 · Digues rompues\",\n"
  "  \"body\": \"Dans la nuit du 31 janvier, une tempête de la mer du Nord rompt les digues néerlandaises. Plus de 1 800 personnes meurent. Les Pays-Bas répondent par le plan Delta.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1943\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1943 · Barrages brisés\",\n"
  "  \"body\": \"Dans la nuit du 16 au 17 mai, des bombardiers britanniques font sauter les barrages de la Möhne et de l’Eder. La vague tue plus de 1 600 personnes, dont beaucoup de travailleurs forcés.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1935\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1935 · Dimanche noir\",\n"
  "  \"body\": \"Le 14 avril, le Dimanche noir, une gigantesque tempête de poussière balaie les plaines américaines frappées par la sécheresse. Elle devient le symbole du Dust Bowl.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1917\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1917 · Halifax\",\n"
  "  \"body\": \"Le 6 décembre, après une collision, un navire chargé de munitions explose dans le port d’Halifax. L’explosion soulève un tsunami dans le port et rase une partie de la ville.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1916\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1916 · Zuiderzee\",\n"
  "  \"body\": \"Dans la nuit du 13 au 14 janvier, une tempête rompt les digues du Zuiderzee. Elle convainc les Néerlandais de fermer cette mer pour de bon.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1910\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1910 · Paris noyé\",\n"
  "  \"body\": \"En janvier, la Seine atteint plus de huit mètres à Paris. Pendant des semaines, les Parisiens circulent en barque et sur des passerelles de bois.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1900\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1900 · Galveston\",\n"
  "  \"body\": \"Le 8 septembre, un ouragan pousse une onde de tempête sur Galveston, au Texas. Entre 6 000 et 12 000 personnes meurent, ce qui reste la catastrophe naturelle la plus meurtrière des États-Unis.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1889\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1889 · Johnstown\",\n"
  "  \"body\": \"Le 31 mai, le barrage de South Fork cède au-dessus de Johnstown, en Pennsylvanie. La crue tue plus de 2 200 personnes dans la vallée.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1883\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1883 · Krakatoa\",\n"
  "  \"body\": \"Le 27 août, le Krakatoa explose. Ses tsunamis tuent plus de 36 000 personnes sur les côtes de Java et de Sumatra.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-1868\",\n"
  "  \"section\": \"chronicle\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"1868 · Wateree\",\n"
  "  \"body\": \"Le 13 août, un séisme au large d’Arica, alors péruvienne, déclenche un tsunami qui emporte le navire de guerre américain Wateree loin dans les terres, où il se pose bien droit.\\nSource : Wikipédia (EN)\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1968\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1968 · Forage profond\",\n"
  "  \"question\": \"Que confirment bientôt les carottes du Glomar Challenger ?\",\n"
  "  \"answer\": \"Que les fonds océaniques sont en expansion.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1960\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1960 · Challenger Deep\",\n"
  "  \"question\": \"Qui a atteint le fond de la fosse Challenger en 1960 ?\",\n"
  "  \"answer\": \"Jacques Piccard et Don Walsh, à bord du bathyscaphe Trieste, à près de 11 000 mètres.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1959\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1959 · Antarctique\",\n"
  "  \"question\": \"Quelle part environ de l’eau douce du monde est retenue dans la glace de l’Antarctique ?\",\n"
  "  \"answer\": \"Environ 70 %.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1948\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1948 · Sel par avion\",\n"
  "  \"question\": \"Pendant le pont aérien de Berlin, que transportaient les hydravions britanniques, et pourquoi eux ?\",\n"
  "  \"answer\": \"Le sel. Leurs coques étaient conçues pour résister à l’eau salée.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1934\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1934 · Bathysphère\",\n"
  "  \"question\": \"À quelle profondeur Beebe et Barton sont-ils descendus en 1934 ?\",\n"
  "  \"answer\": \"923 mètres, dans leur bathysphère d’acier au large des Bermudes.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1920\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1920 · Grand Berlin\",\n"
  "  \"question\": \"Quelles eaux Berlin a-t-elle gagnées du jour au lendemain le 1er octobre 1920 ?\",\n"
  "  \"answer\": \"Le Müggelsee, le Tegeler See et les lacs de la Havel, avec la naissance du Grand Berlin.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1896\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1896 · Fram\",\n"
  "  \"question\": \"Qu’a prouvé la dérive de trois ans du Fram dans la glace arctique ?\",\n"
  "  \"answer\": \"Qu’un courant traverse la mer polaire.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1894\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1894 · Pompes de rue\",\n"
  "  \"question\": \"Qui a dessiné les pompes de rue en fonte installées à Berlin à partir de 1894 ?\",\n"
  "  \"answer\": \"Otto Stahn. Les pompes étaient coulées à Lauchhammer.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1892\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1892 · Choléra\",\n"
  "  \"question\": \"Pourquoi Altona a-t-elle été largement épargnée par le choléra de Hambourg en 1892 ?\",\n"
  "  \"answer\": \"Elle filtrait son eau. Hambourg buvait l’eau de l’Elbe sans la filtrer.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1884\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1884 · Koch\",\n"
  "  \"question\": \"Quel médecin berlinois a identifié le bacille du choléra, et où ?\",\n"
  "  \"answer\": \"Robert Koch, à Calcutta, en 1884.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1856\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1856 · Première usine\",\n"
  "  \"question\": \"Où la première usine des eaux de Berlin a-t-elle ouvert en 1856 ?\",\n"
  "  \"answer\": \"Au Stralauer Tor. Elle a été construite par une compagnie anglaise.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"q-wf-1854\",\n"
  "  \"section\": \"test\",\n"
  "  \"type\": \"prompt\",\n"
  "  \"title\": \"1854 · Broad Street\",\n"
  "  \"question\": \"Comment John Snow a-t-il relié le choléra à une seule pompe en 1854 ?\",\n"
  "  \"answer\": \"Il a cartographié les décès à Soho. Ils menaient à la pompe de Broad Street, dont on a retiré le levier.\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-qr-quellcode\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"Quellcode\",\n"
  "  \"text\": \"Quellcode cartographie les 1968 pompes de rue de Berlin. Chaque pompe vérifiée sur place ouvre une année de plus d’histoire de l’eau.\",\n"
  "  \"url\": \"https://quellcode-berlin.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"wf-qr-full\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"qr\",\n"
  "  \"title\": \"W@terfacts! complet\",\n"
  "  \"text\": \"Le W@terfacts! complet grandit sur la station E!nkster Editions : un fait pour chaque année ouverte dans Quellcode.\",\n"
  "  \"url\": \"https://einkster-editions.netlify.app\"\n"
  " },\n"
  " {\n"
  "  \"id\": \"colophon\",\n"
  "  \"section\": \"more\",\n"
  "  \"type\": \"ref\",\n"
  "  \"title\": \"W@terfacts! Lite\",\n"
  "  \"body\": \"Écrit par Julien et Claude, l’IA\"\n"
  " }\n"
  "]\n";
