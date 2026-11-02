// E!nkster 397 bench: runs the sketch itself, one process per wake (as on the board: ordinary memory starts fresh at
// every wake; what survives is the RTC state, the settings, the flash files and the paper).
// Build and run: ./build.sh  (see build.sh). Pictures go to out/, one .pgm per snapshot; sheet.py makes a contact sheet.
#define EK_HOST
#include "host.h"
#include "einkster397.ino"
#include <sys/wait.h>
#include <sys/stat.h>

uint64_t hostUs = 0, hostWakeUs = 0;
static pid_t mainPid = 0;
extern "C" char *utoa(unsigned v, char *s, int base) { char t[40]; int n = 0; do { int d = v % base; t[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; } while (v); int i = 0; while (n) s[i++] = t[--n]; s[i] = 0; return s; }
extern "C" char *itoa(int v, char *s, int base) { if (v < 0 && base == 10) { *s = '-'; utoa((unsigned)(-(long)v), s + 1, base); return s; } return utoa((unsigned)v, s, base); }
HostHw hw;
std::string hostFs;
std::map<std::string, std::string> hostPrefs;
static std::string outDir = "out", devDir;

// ---------------------------------------------------------------- the faked board, inside a wake
void delay(uint32_t ms) {
  hostUs += (uint64_t)ms * 1000;
  if (hostUs - hostWakeUs > 30ULL * 60 * 1000000) { fprintf(stderr, "a wake ran for 30 minutes without sleeping\n"); _exit(3); }
}
void epdPushRows(int r0, int r1, const uint8_t *old) {
  size_t a = (size_t)r0 * (NW / 8), n = (size_t)(r1 - r0 + 1) * (NW / 8);
  if (!hw.sceneEdit && memcmp(old + a, hw.paper + a, n)) hw.badPartials++;   // the window's old picture must be what the paper shows
  hw.sceneEdit = false;
  memcpy(hw.paper + a, fb + a, n);
  hw.windows++; hw.windowRows = r1 - r0 + 1; hw.pushes++;
  delay(400);                                   // the panel's partial waveform takes time (not measured: a stand-in)
}
void epdPush(bool full, const uint8_t *old) {
  if (!full && !hw.sceneEdit && memcmp(old, hw.paper, sizeof hw.paper)) { hw.badPartials++; if (getenv("EK_VERBOSE")) { int d = 0; for (int i = 0; i < 48000; i++) d += old[i] != hw.paper[i]; printf("    (bad partial: scr %d tile %d rot %d, %d bytes differ, wake cause %d)\n", R.scr, R.tile, rot, d, hw.wakeCause); } }    // the old picture must be what the paper shows
  hw.sceneEdit = false;
  memcpy(hw.paper, fb, sizeof hw.paper);
  if (R.scr == SCR_SETUP) { extern void snap(const char *); snap("27-setup"); }
  if (full) hw.fullRefreshes++; else hw.partRefreshes++;
  hw.pushes++;
}
static void saveState();
void sleepFor(uint64_t us) { hw.slept = true; hw.sleepUs = us; saveState(); _exit(0); }
size_t fsSize(const char *p) { FILE *f = fopen((hostFs + p).c_str(), "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); size_t n = ftell(f); fclose(f); return n; }
void fsAppend(const char *p, const char *d, size_t n) { FILE *f = fopen((hostFs + p).c_str(), "ab"); if (f) { fwrite(d, 1, n, f); fclose(f); } }
size_t fsRead(const char *p, uint8_t **out) {   // v0.3.0
  *out = NULL; FILE *f = fopen((hostFs + p).c_str(), "rb"); if (!f) return 0;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  *out = (uint8_t *)malloc(n ? n : 1); size_t k = fread(*out, 1, n, f); fclose(f); return k;
}
void fsRotate(const char *p, const char *old) { remove((hostFs + old).c_str()); rename((hostFs + p).c_str(), (hostFs + old).c_str()); }

// ---------------------------------------------------------------- state between wakes
static std::string stateFile() { return devDir + "/state.bin"; }
static void putStr(FILE *f, const std::string &s) { uint32_t n = s.size(); fwrite(&n, 4, 1, f); fwrite(s.data(), 1, n, f); }
static std::string getStr(FILE *f) { uint32_t n = 0; if (fread(&n, 4, 1, f) != 1) return ""; std::string s(n, 0); if (n && fread(&s[0], 1, n, f) != n) return ""; return s; }
static void saveState() {
  FILE *f = fopen(stateFile().c_str(), "wb");
  fwrite(&hostUs, 8, 1, f); fwrite(&hw, sizeof hw, 1, f); fwrite(&R, sizeof R, 1, f);
  fwrite(flRing, sizeof flRing, 1, f); fwrite(&flLen, 2, 1, f); fwrite(&flMagic, 4, 1, f); fwrite(&flRunStartUs, 8, 1, f);
  uint32_t n = hostPrefs.size(); fwrite(&n, 4, 1, f); for (auto &kv : hostPrefs) { putStr(f, kv.first); putStr(f, kv.second); }
  fclose(f);
}
static void loadState() {
  FILE *f = fopen(stateFile().c_str(), "rb"); if (!f) return;
  size_t ok = fread(&hostUs, 8, 1, f); ok += fread(&hw, sizeof hw, 1, f); ok += fread(&R, sizeof R, 1, f);
  ok += fread(flRing, sizeof flRing, 1, f); ok += fread(&flLen, 2, 1, f); ok += fread(&flMagic, 4, 1, f); ok += fread(&flRunStartUs, 8, 1, f);
  uint32_t n = 0; ok += fread(&n, 4, 1, f); hostPrefs.clear(); for (uint32_t i = 0; i < n; i++) { std::string k = getStr(f), v = getStr(f); hostPrefs[k] = v; }
  fclose(f);
}

// ---------------------------------------------------------------- the driver
static int checks = 0, fails = 0, wakesRun = 0;
#define OK(c, what) do { checks++; bool _c = (c); if (!_c) fails++; printf("  %s %s\n", _c ? "ok  " : "FAIL", what); } while (0)
struct Ev { uint32_t at, hold; int pin; };
static void runWake(int cause, int pin, std::vector<Ev> evs) {
  hw.wakeCause = cause; hw.wakePin = pin; hw.slept = false; hw.nEv = 0;
  for (auto &e : evs) if (hw.nEv < 16) hw.ev[hw.nEv++] = { e.at, e.hold, e.pin };
  saveState(); fflush(stdout);
  pid_t p = fork();
  if (p == 0) {
    loadState();
    setRot(0); havePrev = false;   // a real wake starts from fresh statics: upright, nothing drawn yet
    hostWakeUs = hostUs;
    if (cause == 0) { hw.bootUs = hostUs; hw.sysValid = false; }
    setup();
    for (;;) loop();
  }
  int st = 0; waitpid(p, &st, 0); wakesRun++;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("  FAIL the wake crashed or did not sleep (status %d)\n", st); fails++; checks++; }
  loadState();
}
static void newDevice(const char *name) {
  devDir = outDir + "/" + name; std::string cmd = "rm -rf " + devDir + " && mkdir -p " + devDir; if (system(cmd.c_str())) {}
  hostFs = devDir; hostPrefs.clear(); memset(&hw, 0, sizeof hw); memset(&R, 0, sizeof R); flLen = 0; flMagic = 0; flRunStartUs = 0;
  memset(hw.paper, 0xFF, sizeof hw.paper);
  hostUs = 1793016000ULL * 1000000;            // Monday 26 October 2026, 12:00 world time (13:00 in Berlin)
  hw.chipValid = true; hw.batMv = 3950; hw.batPct = 82; hw.roomThere = true; hw.roomC = 21.4f; hw.roomRH = 47;
}
static void powerOn() { runWake(0, -1, {}); }
static void dialWake(int pin, uint32_t hold = 120, std::vector<Ev> more = {}) {
  hostUs += 5000000;                            // five seconds pass, then a finger
  std::vector<Ev> e = { { 0, hold, pin } }; for (auto &m : more) e.push_back(m);
  runWake(2, pin, e);
}
static void timerWake() { hostUs += hw.sleepUs ? hw.sleepUs : 60000000ULL; runWake(4, -1, {}); }
static int quiet(uint64_t seconds) {           // timer wakes only, for this long
  uint64_t end = hostUs + seconds * 1000000ULL; int n = 0;
  while (!hw.off && hostUs + (hw.sleepUs ? hw.sleepUs : 60000000ULL) <= end) { timerWake(); n++; }
  hostUs = end > hostUs ? end : hostUs;
  return n;
}
// the driver's own idea of how the page is turned: the typewriter's pages lie on their side (v0.2.0)
static void parentRot() { if (R.scr == SCR_TYPE || R.scr == SCR_KB) setRot(hostPrefs.count("twRot") && hostPrefs["twRot"] == "2" ? 2 : 1); else if (getpid() == mainPid) setRot(0); }
void snap(const char *label) {
  parentRot();
  std::string p = outDir + "/" + label + ".pgm";
  FILE *f = fopen(p.c_str(), "wb"); fprintf(f, "P5\n%d %d\n255\n", W, H);
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) fputc(inkAt(hw.paper, x, y) ? 0 : 255, f);
  fclose(f);
}
static bool paperInk(int x0, int y0, int x1, int y1) {
  for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (inkAt(hw.paper, x, y)) return true;
  return false;
}
static bool marginsClear() {
  parentRot(); return !paperInk(0, 0, 6, H) && !paperInk(W - 6, 0, W, H) && !paperInk(0, H - 4, W, H); }
static std::string fieldLog() {
  std::string t; for (const char *p : { "/field.old", "/field.txt" }) { FILE *f = fopen((hostFs + p).c_str(), "rb"); if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); } }
  if (flMagic == FL_MAGIC) t.append(flRing, flLen);
  return t;
}
static bool has(const std::string &t, const char *x) { return t.find(x) != std::string::npos; }
static int count(const std::string &t, const char *x) { int n = 0; for (size_t p = t.find(x); p != std::string::npos; p = t.find(x, p + 1)) n++; return n; }
// a card of a given type in a pack, found by a helper process (the driver's own memory stays untouched)
static int findCard(const char *slug, const char *type, int skip = 0) {
  int fd[2]; if (pipe(fd)) return -1;
  pid_t p = fork();
  if (p == 0) {
    int r = -1;
    if (loadPack(slug)) { pickSection(-1); for (int i = 0; i < nIn; i++) if (!strcmp(jgs(cardN[inSec[i]], "type"), type) && skip-- == 0) { r = i; break; } }
    if (write(fd[1], &r, sizeof r)) {} _exit(0);
  }
  int r = -1; if (read(fd[0], &r, sizeof r)) {} waitpid(p, NULL, 0); close(fd[0]); close(fd[1]); return r;
}

// ---------------------------------------------------------------- the scenes
static void sceneFirstLight() {
  printf("first light: power on, the clock, the weather, a day of minutes\n");
  newDevice("first");
  powerOn(); snap("01-first-clock");
  std::string fl = fieldLog();
  OK(hw.slept && R.scr == SCR_TILE && R.tile == T_CLOCK, "power on: the Clock tile, then sleep");
  OK(hw.fullRefreshes == 2 && hw.badPartials == 0, "two clean refreshes at power on (the name, then the face)");
  OK(has(fl, "app       einkster397") && has(fl, "version   v0.4.0") && has(fl, "role      classic"), "the field log opens a run: app, version, role");
  OK(hostPrefs["wN"] == "2" && hostPrefs["wSeeded"] == "1", "the Classic's two networks move into the settings, once");
  OK(!has(fl, "Bench") && !has(fl, "bench-pass"), "no network name or password in the field log");
  OK(hw.sleepUs <= 61000000ULL && hw.sleepUs > 0, "on the Clock tile it wakes for the next minute");
  OK(marginsClear(), "nothing on the paper's edges");
  // the weather arrives (the bench has no network: the values are put where the fetch puts them)
  R.wxAt = (uint32_t)(hostUs / 1000000); R.outT10 = 94; R.outCode = 61; R.dMax10[0] = 121; R.dMin10[0] = 68; R.dCode[0] = 61;
  R.dMax10[1] = 110; R.dMin10[1] = 51; R.dCode[1] = 3; R.dMax10[2] = 135; R.dMin10[2] = 72; R.dCode[2] = 1; R.lastFetch = R.wxAt;
  int full0 = hw.fullRefreshes;
  int n = quiet(5 * 60); snap("02-clock-weather");
  OK(n >= 5 && hw.badPartials == 0, "five minutes: a partial refresh each minute, each from what the paper shows");
  OK(hw.fullRefreshes == full0, "no flashing between the hours");
  n = quiet(3 * 3600);
  OK(hw.fullRefreshes - full0 >= 3 && hw.fullRefreshes - full0 <= 4 && hw.badPartials == 0, "over 3 hours: one clean refresh at each full hour");
  fl = fieldLog();
  OK(count(fl, "[battery]") >= 6, "a battery and room line every 30 minutes");
  OK(has(fl, "room 21.4\xC2\xB0 47 %"), "the room's climate goes with it");
  OK(!R.wxOk, "after 3 hours the weather is too old to show");
}
static void sceneDial() {
  printf("the dial: tiles, the shelf, a card, a sleep inside a card, back\n");
  newDevice("dial");
  powerOn();
  R.wxAt = (uint32_t)(hostUs / 1000000); R.outT10 = 94; R.outCode = 61; R.dMax10[0] = 121; R.dMin10[0] = 68; R.dCode[0] = 61;
  R.dMax10[1] = 110; R.dMin10[1] = 51; R.dCode[1] = 3; R.dMax10[2] = 135; R.dMin10[2] = 72; R.dCode[2] = 1; R.lastFetch = R.wxAt;
  dialWake(PIN_DOWN); snap("03-climate");
  OK(R.tile == T_CLIMATE && hw.badPartials == 0, "a turn down wakes it and moves to Climate, a partial refresh from the paper");
  OK(marginsClear(), "Climate fits the paper");
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_DOWN }, { 4000, 120, PIN_DOWN }, { 6000, 120, PIN_DOWN }, { 8000, 120, PIN_DOWN } });   // the waking turn counts: five moves
  OK(R.tile == T_DEVICE, "five more turns: Editions, Typewriter, Archives, Catalog, Device");
  snap("06-device");
  OK(marginsClear(), "Device fits the paper");
  dialWake(PIN_UP, 120, { { 2000, 120, PIN_UP }, { 4000, 120, PIN_UP }, { 6000, 120, PIN_UP } });
  OK(R.tile == T_EDITIONS, "four turns up: Editions"); snap("04-editions");
  // the shelf, a pack, a section, a card
  dialWake(PIN_PRESS); snap("07-shelf");
  OK(R.scr == SCR_LIST && R.list == L_SHELF, "press on Editions: the shelf");
  OK(marginsClear(), "the shelf fits the paper");
  dialWake(PIN_PRESS); snap("08-sections");
  OK(R.scr == SCR_LIST && R.list == L_SECS && !strcmp(R.slug, "atlantidia-lite"), "press: Atlantid!a Lite and its sections");
  dialWake(PIN_PRESS); snap("09-cards");
  OK(R.list == L_CARDS && R.sec == 0, "press: the first section's cards");
  dialWake(PIN_PRESS); snap("10-card");
  OK(R.scr == SCR_CARD && R.card == 0 && hw.badPartials == 0, "press: the first card, every wake redrawn from what the paper shows");
  OK(marginsClear(), "a card fits the paper");
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_DOWN } }); snap("11-card-next");
  OK(R.scr == SCR_CARD && (R.card > 0 || R.page > 0), "turns down: the next page or card");
  uint32_t sl = (uint32_t)(hw.sleepUs / 1000000);
  OK(sl > 60 && sl <= 1802, "inside a card it sleeps until the next half hour, not the next minute");
  int before = hw.pushes; quiet(3600);
  OK(hw.pushes == before, "an hour asleep inside a card: the page stays, nothing is redrawn");
  dialWake(PIN_DOWN);
  OK(R.scr == SCR_CARD && hw.badPartials == 0, "woken an hour later inside the card: the pack is read again and the page moves");
  // back up: list, sections, shelf, tile
  dialWake(PIN_PRESS, 1000); OK(R.scr == SCR_LIST && R.list == L_CARDS, "long press: back to the cards");
  dialWake(PIN_PRESS, 1000); OK(R.list == L_SECS, "long press: back to the sections");
  dialWake(PIN_PRESS, 1000); OK(R.list == L_SHELF, "long press: back to the shelf");
  dialWake(PIN_PRESS, 1000); OK(R.scr == SCR_TILE && R.tile == T_EDITIONS, "long press: back to the tile");
  dialWake(PIN_PRESS, 1000); OK(R.tile == T_CLOCK, "long press on a tile: the Clock");
  OK(hw.badPartials == 0, "no partial refresh ever started from a wrong picture");
}
static void sceneCards() {
  printf("the card kinds: a question and its answer, a checklist, a code, the colophon\n");
  newDevice("cards");
  powerOn();
  int q = findCard("yogia-lite", "prompt"), c = findCard("yogia-lite", "checklist"), r = findCard("yogia-lite", "qr");
  OK(q >= 0 && c >= 0 && r >= 0, "Yog!a Lite has a question, a checklist and a code");
  auto openCard = [&](int i) { hw.sceneEdit = true; R.scr = SCR_CARD; strcpy(R.slug, "yogia-lite"); R.sec = -1; R.card = i; R.page = 0; R.revealed = 0; R.list = L_CARDS; };
  openCard(q); dialWake(PIN_PRESS, 1000); dialWake(PIN_PRESS); snap("12-question");   // back to the list and in again
  OK(R.scr == SCR_CARD, "the question is open");
  openCard(q); dialWake(PIN_DOWN); dialWake(PIN_UP);
  OK(R.card == q && !R.revealed, "a question shows without its answer");
  dialWake(PIN_PRESS); snap("13-answer");
  OK(R.revealed && R.scr == SCR_CARD, "press: the answer");
  openCard(c); dialWake(PIN_DOWN); dialWake(PIN_UP); snap("14-checklist");
  OK(R.card == c, "a checklist");
  openCard(r); dialWake(PIN_DOWN); dialWake(PIN_UP); snap("15-qr-card");
  OK(R.card == r && marginsClear(), "a card with a code fits the paper");
  // the last card of every pack is its colophon
  OK(findCard("guineapedia-lite", "ref") >= 0, "Guineapedi@ Lite opens");
  OK(hw.badPartials == 0, "every partial refresh started from the paper");
}
static void sceneCatalogDevice() {
  printf("the catalog, the device menu, French, about, switching off\n");
  newDevice("menus");
  powerOn();
  dialWake(PIN_UP, 120, { { 2000, 120, PIN_UP } }); OK(R.tile == T_CATALOG, "two turns up from the Clock: Catalog"); snap("16-catalog");
  OK(marginsClear(), "Catalog fits the paper");
  dialWake(PIN_PRESS); snap("17-catalog-list");
  OK(R.scr == SCR_LIST && R.list == L_CATALOG && R.cur == 0, "the list opens");
  dialWake(PIN_PRESS); snap("18-business-card");
  OK(R.scr == SCR_QR && R.qr == 0, "first row: the business card, from the build secret");
  dialWake(PIN_DOWN); snap("19-station-code");
  OK(R.qr == 1 && marginsClear(), "turn: the next station's code");
  dialWake(PIN_PRESS); dialWake(PIN_PRESS, 1000);
  OK(R.scr == SCR_TILE && R.tile == T_CATALOG, "press, then long press: back to the tile");
  dialWake(PIN_UP, 120, { { 2000, 120, PIN_UP } }); OK(R.tile == T_TYPE, "two up: Archives, Typewriter");
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_DOWN }, { 4000, 120, PIN_DOWN } }); OK(R.tile == T_DEVICE, "three down: Device");
  dialWake(PIN_PRESS); snap("20-device-menu");
  OK(R.scr == SCR_LIST && R.list == L_DEVICE, "press: the device menu");
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_PRESS } }); snap("21-menu-french");
  OK(hostPrefs["lang"] == "fr" && R.fr, "Language: one press switches to French, stored as lang");
  OK(has(fieldLog(), "interface language set to fr"), "the switch is logged");
  dialWake(PIN_PRESS, 1000); dialWake(PIN_PRESS, 1000); snap("22-clock-french");
  OK(R.tile == T_CLOCK && marginsClear(), "the Clock in French fits the paper");
  dialWake(PIN_UP); snap("23-device-french"); dialWake(PIN_DOWN); dialWake(PIN_DOWN); snap("24-climate-french");
  OK(R.tile == T_CLIMATE, "Device, Clock, Climate");
  OK(marginsClear(), "Device and Climate in French fit the paper");
  // about
  hw.sceneEdit = true; R.scr = SCR_LIST; R.list = L_DEVICE; R.cur = 4; dialWake(PIN_DOWN); dialWake(PIN_UP); dialWake(PIN_PRESS); snap("25-about");
  OK(R.scr == SCR_DOC && marginsClear(), "About and credits fit the paper");
  dialWake(PIN_PRESS);
  // back to English, then off
  dialWake(PIN_UP, 120, { { 2000, 120, PIN_UP }, { 4000, 120, PIN_UP }, { 6000, 120, PIN_PRESS } });
  OK(hostPrefs["lang"] == "en", "and back to English");
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_DOWN }, { 4000, 120, PIN_DOWN }, { 6000, 120, PIN_DOWN }, { 8000, 120, PIN_DOWN }, { 10000, 120, PIN_PRESS } }); snap("26-off");
  OK(hw.off, "Switch off: the power chip turns it off");
  OK(has(fieldLog(), "[end] switched off: Device menu"), "the run ends cleanly in the field log");
}
static void sceneSetupScreen() {
  printf("the setup screen (the hotspot itself needs the board)\n");
  newDevice("setup");
  powerOn();
  hw.sceneEdit = true; R.scr = SCR_LIST; R.list = L_DEVICE; R.cur = 1; R.tile = T_DEVICE;
  dialWake(PIN_UP); OK(R.cur == 0, "the menu's first row");
  hw.slept = false;
  dialWake(PIN_PRESS);
  OK(hw.badPartials == 0, "the setup screen is drawn, then the Device tile again, each from the paper");
  OK(R.scr == SCR_TILE && R.tile == T_DEVICE, "after the setup page: the Device tile");
}
static void sceneNoClock() {
  printf("no clock: a board whose clock chip lost the time\n");
  newDevice("noclock"); hw.chipValid = false;
  powerOn(); snap("28-no-clock");
  OK(R.faceMin == 0 && marginsClear(), "the Clock tile says the clock is not set, and fits");
  OK(hw.sleepUs > 61000000ULL, "without a clock it does not wake every minute");
}
static void sceneBattery() {
  printf("the battery: empty, and a board without a cell\n");
  newDevice("battery");
  powerOn();
  hw.batMv = 3420; hw.batPct = 2;
  quiet(31 * 60); snap("29-battery-empty");
  OK(hw.off, "an empty cell: it says so and switches off");
  OK(has(fieldLog(), "[battery] empty: 3.42 V"), "logged with the voltage");
  newDevice("usbonly"); hw.batMv = 0; hw.usb = true;
  powerOn(); snap("30-usb-only");
  OK(hw.slept && !hw.off && marginsClear(), "no cell, USB only: it runs and says USB");
}

// ---------------------------------------------------------------- v0.2.0: the typewriter
struct K { uint8_t mod, usage; };
static void keysAt(uint32_t from, std::vector<K> ks, uint32_t gap = 180) {
  hw.nKeys = 0;
  for (auto &k : ks) { if (hw.nKeys + 2 > 400) break; hw.keys[hw.nKeys++] = { from, k.mod, k.usage, false, false }; hw.keys[hw.nKeys++] = { from + 70, 0, 0, true, false }; from += gap; }
}
static std::string sdFile(const char *name) { std::string t; FILE *f = fopen((hostFs + "/sd/typed/" + name).c_str(), "rb"); if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); } return t; }
static void sceneTypewriter() {
  printf("the typewriter: pair, type German, save, sleep, a cut mid-line, no card, the page turned\n");
  newDevice("type"); hw.sdThere = true; hw.kbThere = true; hw.kbBat = 87;
  powerOn(); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows);
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_DOWN }, { 4000, 120, PIN_DOWN } }); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows); snap("31-type-tile");
  OK(R.tile == T_TYPE && marginsClear(), "three turns: the Typewriter tile, which fits the paper");
  // press: no keyboard yet, so the list; a press pairs; then Grüße, Enter, and the German layer at work
  std::vector<K> ks = { { 0x02, 0x0A }, { 0, 0x15 }, { 0, 0x2F }, { 0, 0x2D }, { 0, 0x08 }, { 0, 0x28 },
    { 0, 0x35 }, { 0, 0x08 }, { 0, 0x2E }, { 0, 0x08 }, { 0x02, 0x2E }, { 0, 0x04 }, { 0, 0x2C },
    { 0, 0x39 }, { 0, 0x04 }, { 0, 0x05 }, { 0, 0x39 }, { 0, 0x2C }, { 0, 0x1B }, { 0, 0x1D }, { 0, 0x2A }, { 0x40, 0x08 }, { 0, 0x35 }, { 0, 0x2C }, { 0, 0x28 },
    { 0x02, 0x1D }, { 0, 0x08 }, { 0, 0x16 } };   // and "Yes" stays on the line, unsaved
  keysAt(5000, ks);
  hw.windows = 0; int fullT0 = hw.fullRefreshes;
  dialWake(PIN_PRESS, 120, { { 2500, 120, PIN_PRESS } }); if (getenv("EK_VERBOSE")) printf("    [full refreshes in the typing wake: %d]\n", hw.fullRefreshes - fullT0); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows);
  snap("32-type-page");
  OK(hw.fullRefreshes - fullT0 >= 5, "each Enter draws the page clean (two Enters: two clean pages beyond the pairing ones)");   // v0.2.1
  std::string day = sdFile("2026-10-26.txt");
  OK(day == "Gr\xC3\xBC\xC3\x9F" "e\n\xC3\xAA\xC3\xA9\xC3\xA0 AB x\xE2\x82\xAC^\n", "two lines on the card: Gr\xC3\xBC\xC3\x9F" "e, then the dead keys, Caps Lock, Backspace and AltGr");
  OK(!strcmp(R.tline, "Yes"), "the line not yet saved stays in memory through the sleep");
  OK(hostPrefs.count("kbA") && hw.kbReports >= 50, "paired and bonded; every report arrived");
  OK(hw.windows >= 3 && hw.badPartials == 0, "keys reach the paper as window refreshes, each from what the paper shows");
  std::string fl = fieldLog();
  OK(has(fl, "[keyboard] paired and bonded") && !has(fl, "bench keyboard"), "the pairing is logged, never the keyboard's name");
  OK(has(fl, "[type] session ended after 10 min without a key: ") && has(fl, " 2 lines saved, ") && has(fl, "key to paper median"), "one line for the session: length, lines saved, key to paper");
  OK(!has(fl, "Gr\xC3\xBC\xC3\x9F" "e") && !has(fl, "Yes"), "never the words");
  OK(hw.slept && R.scr == SCR_TYPE && marginsClear(), "asleep after 10 minutes, the page still on the paper, inside the margins");
  // the dial wakes it: the keyboard comes back with no new pairing, Enter saves "Yes"
  keysAt(3000, { { 0, 0x28 } }); int full0 = hw.fullRefreshes;
  dialWake(PIN_PRESS); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows);
  OK(sdFile("2026-10-26.txt").find("Yes\n") != std::string::npos, "woken by the dial, the keyboard back, the kept line saved");
  OK(has(fieldLog(), "[keyboard] back in") && hw.fullRefreshes > full0, "logged as back with no new pairing; the page drawn clean");
  // a power cut in the middle of a line: the saved lines stay
  keysAt(3000, { { 0, 0x04 }, { 0, 0x05 }, { 0, 0x06 } }); hw.cutAtKey = 3;
  dialWake(PIN_PRESS); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows);
  hw.cutAtKey = 0;
  powerOn(); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows);
  day = sdFile("2026-10-26.txt");
  OK(day == "Gr\xC3\xBC\xC3\x9F" "e\n\xC3\xAA\xC3\xA9\xC3\xA0 AB x\xE2\x82\xAC^\nYes\n", "a power cut mid-line: the three saved lines are intact, the cut line is gone");
  OK(has(fieldLog(), "had no clean end"), "the cut is in the log");
  // no card: the line is not saved, and the page says so
  hw.sdThere = false; hw.nKeys = 0;
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_DOWN }, { 4000, 120, PIN_DOWN } });
  keysAt(3000, { { 0, 0x0B }, { 0, 0x0C }, { 0, 0x28 } });               // v0.4.0: typed in the typewriter (on the tiles keys act too) if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows);
  OK(R.tile == T_TYPE, "back on the Typewriter tile");
  dialWake(PIN_PRESS); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows); snap("33-type-no-card");
  OK(sdFile("2026-10-26.txt").find("hi") == std::string::npos && has(fieldLog(), "no microSD"), "no card: nothing written, said in the log");
  // the page turned the other way: Device > Typewriter: turn the page
  hw.sdThere = true; hw.nKeys = 0;
  hw.sceneEdit = true; R.scr = SCR_LIST; R.list = L_DEVICE; R.cur = 2; R.tile = T_DEVICE; setRot(0);
  dialWake(PIN_PRESS); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows);
  OK(hostPrefs["twRot"] == "2", "the menu turns the typewriter's page");
  hw.sceneEdit = true; R.scr = SCR_TILE; R.tile = T_TYPE;
  keysAt(3000, { { 0x02, 0x0B }, { 0, 0x0C } });
  dialWake(PIN_PRESS); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows); snap("34-type-turned");
  if (getenv("EK_VERBOSE")) printf("  (bad partials %d, margins %d, scr %d, rot pref %s)\n", hw.badPartials, marginsClear(), R.scr, hostPrefs["twRot"].c_str());
  OK(hw.badPartials == 0 && marginsClear(), "the turned page fits, and every refresh started from the paper");
  hw.sceneEdit = true; R.scr = SCR_TILE; R.tile = T_DEVICE; setRot(0); dialWake(PIN_PRESS, 120, { { 2000, 120, PIN_PRESS } }); if (getenv("EK_VERBOSE")) printf("    [line %d: bad %d, windows %d]\n", __LINE__, hw.badPartials, hw.windows); snap("35-device-keyboard");
}

// ---------------------------------------------------------------- v0.3.0: the archives
static std::vector<K> typeStr(const char *t) {           // ASCII on the German layout (y and z swapped), lower case
  std::vector<K> v;
  for (const char *c = t; *c; c++) {
    if (*c >= 'a' && *c <= 'z') v.push_back({ 0, (uint8_t)(*c == 'y' ? 0x1D : *c == 'z' ? 0x1C : 0x04 + (*c - 'a')) });
    else if (*c >= '1' && *c <= '9') v.push_back({ 0, (uint8_t)(0x1E + (*c - '1')) });
    else if (*c == '0') v.push_back({ 0, 0x27 });
    else if (*c == ' ') v.push_back({ 0, 0x2C });
  }
  return v;
}
static void shelfCard() { std::string c = "mkdir -p " + hostFs + "/sd && cp -r fixtures/shelf " + hostFs + "/sd/"; if (system(c.c_str())) {} }
static void parentRows() { shALoaded = ""; shRowsFor = ""; shRowsAt = 0xFFFFFFFF; shClose(); shFill(); }
static void sceneShelf() {
  printf("the archives: the card, the letter wheel, reading, sleep, the keyboard, the search, no card\n");
  newDevice("shelf"); hw.sdThere = true; shelfCard();
  powerOn();
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_DOWN }, { 4000, 120, PIN_DOWN }, { 6000, 120, PIN_DOWN } }); snap("36-archives-tile");
  OK(R.tile == T_SHELF && marginsClear(), "four turns: the Archives tile, which fits the paper");
  dialWake(PIN_PRESS); snap("37-archives-list");
  OK(R.scr == SCR_LIST && R.list == L_ARCH && R.shN == 2 && R.shCard == 1, "press: the card holds two archives (the one from another converter version is left out)");
  dialWake(PIN_PRESS); snap("38-find-letter");
  OK(R.scr == SCR_FIND && !strcmp(R.shArc, "bench_en") && !strcmp(R.shQ, "a") && R.shFocus == 0 && marginsClear(), "press: the first archive, the letter wheel at A");
  dialWake(PIN_DOWN);
  parentRows();
  OK(!strcmp(R.shKey, "b") && shNRows >= 1 && shRows[0].title == "Berlin", "turn: B, and Berlin is first");
  dialWake(PIN_PRESS); dialWake(PIN_PRESS); snap("39-read-berlin");
  OK(R.scr == SCR_READ && R.shPage == 0 && rdLoad() && rdTitle == "Berlin" && marginsClear(), "press: the titles, press: Berlin, read upright inside the margins");
  OK(hw.badPartials == 0, "every partial refresh so far started from the paper");
  // the letter L: Lodz (another name), Lódz, Long article
  dialWake(PIN_PRESS); dialWake(PIN_PRESS, 1000);
  OK(R.scr == SCR_FIND && R.shFocus == 0, "press: back to the titles; long press: back to the letter");
  std::vector<Ev> ten; for (int i = 1; i < 10; i++) ten.push_back({ (uint32_t)(i * 600), 120, PIN_DOWN });
  dialWake(PIN_DOWN, 120, ten);
  parentRows();
  OK(!strcmp(R.shKey, "l") && shNRows == 3 && shRows[0].title == "Lodz" && shRows[0].target == "L\xC3\xB3" "dz" && shRows[2].title == "Long article", "ten turns: L, with the other name pointing to its article");
  snap("40-find-l");
  dialWake(PIN_PRESS, 120, { { 2000, 120, PIN_DOWN }, { 4000, 120, PIN_DOWN }, { 6000, 120, PIN_PRESS } }); snap("41-read-long");
  rdFor = ""; R.shPage = 0;
  OK(R.scr == SCR_READ && rdLoad() && rdPages.size() >= 5, "the long article opens, in five pages or more");
  int pages = rdPages.size();
  dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_DOWN } }); snap("42-read-page3");
  OK(R.shPage == 2 && marginsClear(), "two turns: page 3, inside the margins");
  OK(hw.slept, "45 s without a turn: asleep on the page");
  dialWake(PIN_DOWN);
  OK(R.shPage == 3 && hw.badPartials == 0, "the dial wakes it and turns the page (the article read again from the card)");
  // keep turning past the end: it stops on the last page
  std::vector<Ev> many; for (int i = 1; i < 15; i++) many.push_back({ (uint32_t)(i * 3000), 120, PIN_DOWN });   // the bench keeps 16 events a wake
  for (int w = 0; w < (pages + 2) / 15 + 1; w++) dialWake(PIN_DOWN, 120, many);
  if (getenv("EK_VERBOSE")) printf("    [page %d of %d, slept %d]\n", R.shPage, pages, hw.slept);
  OK(R.shPage == pages - 1, "turning past the end stays on the last page");
  OK(!has(fieldLog(), "Berlin") && !has(fieldLog(), "Long article") && !has(fieldLog(), "bench_en") && has(fieldLog(), "[shelf] article opened"), "the field log says an article was opened, never which");
  // the keyboard: paired before (as by the typewriter), it comes back by itself; type to search
  hostPrefs["kbA"] = "be0000000000"; hostPrefs["kbT"] = "1"; hw.kbThere = true; hw.kbBat = 64;
  dialWake(PIN_PRESS, 1000);
  OK(R.scr == SCR_FIND, "long press: from the page back to the titles");
  std::vector<K> ks = typeStr("gr"); ks.push_back({ 0, 0x2F });          // g r ü
  ks.push_back({ 0, 0x28 });                                             // Enter: open
  keysAt(3000, ks);
  dialWake(PIN_UP); snap("43-read-grusse");
  OK(R.scr == SCR_READ && rdLoad() && rdTitle == "Gr\xC3\xBC\xC3\x9F" "e" && !strcmp(R.shKey, "gru"), "the keyboard comes back by itself; typing gr\xC3\xBC finds Gr\xC3\xBC\xC3\x9F" "e, Enter opens it");
  OK(has(fieldLog(), "[keyboard] back, no new pairing"), "logged as back, with no new pairing");
  // Esc, then a new query: item 2 (the search has 3000 titles to halve), Page Down, then nothing found
  ks = { { 0, 0x29 } };                                                  // Esc: back to the titles
  for (int i = 0; i < 3; i++) ks.push_back({ 0, 0x2A });                 // Backspace x3: the query is empty
  auto it = typeStr("item 2"); ks.insert(ks.end(), it.begin(), it.end());
  keysAt(3000, ks);
  dialWake(PIN_UP); snap("44-find-item2");
  parentRows();
  OK(R.scr == SCR_FIND && !strcmp(R.shQ, "item 2") && shNRows == SH_ROWS && shRows[0].title == "Item 2000" && shMore, "typing \"item 2\": Item 2000 first, of the 3000 titles");
  int at0 = R.shAt;
  keysAt(3000, { { 0, 0x4E }, { 0, 0x4E } });                            // Page Down twice
  dialWake(PIN_UP);
  parentRows();
  OK((int)R.shAt > at0 && strcmp(shRows[R.shCur].title.c_str(), "Item 2010") > 0, "Page Down moves on through the list");
  ks = {}; for (int i = 0; i < 6; i++) ks.push_back({ 0, 0x2A });
  it = typeStr("zzz"); ks.insert(ks.end(), it.begin(), it.end());
  int fz = hw.fullRefreshes, pz = hw.partRefreshes;
  keysAt(3000, ks); dialWake(PIN_UP); snap("45-find-nothing");
  parentRows();
  OK(shNRows == 0 && !strcmp(R.shQ, "zzz"), "zzz: nothing, and the page says so");
  OK(hw.partRefreshes > pz && hw.fullRefreshes > fz, "the list follows each key with a quick refresh, and a clean one when typing pauses");
  // Tab: the other archive, the same query; usa finds United States through its other name
  ks = { { 0, 0x2A }, { 0, 0x2A }, { 0, 0x2A } }; it = typeStr("usa"); ks.insert(ks.end(), it.begin(), it.end()); ks.push_back({ 0, 0x2B }); ks.push_back({ 0, 0x28 });
  keysAt(3000, ks); dialWake(PIN_UP); snap("46-read-usa-split");
  OK(!strcmp(R.shArc, "split") && R.scr == SCR_READ && rdLoad() && rdTitle == "United States" && R.shFile > 0, "Tab: the archive in three files, usa \xE2\x86\x92 United States, read from its third file");
  OK(hw.badPartials == 0, "every partial refresh started from the paper");
  // no card
  hw.sdThere = false; hw.sceneEdit = true; R.scr = SCR_TILE; R.tile = T_SHELF; dialWake(PIN_PRESS); snap("47-archives-no-card");
  OK(R.scr == SCR_TILE && R.shCard == 0 && has(fieldLog(), "[shelf] no microSD") && marginsClear(), "no card: the tile says so, nothing opens");
}

// ---------------------------------------------------------------- v0.3.0: the 30-minute log and the charge state
static void sceneBatLog() {
  printf("the 30-minute log: a drain, a charge with its top-up, the battery page, the table\n");
  newDevice("battery"); hw.batMv = 4100; hw.batPct = 95;
  powerOn();
  for (int i = 0; i < 24; i++) { hw.batMv -= 6; hw.batPct -= 1; quiet(1800); }
  size_t n0 = fsSize("/log.bin");
  OK(n0 >= 24 * 16 && n0 % 16 == 0, "a 16-byte sample every 30 minutes, asleep");
  uint8_t *b = NULL; size_t sz = fsRead("/log.bin", &b); BatSample f0; memcpy(&f0, b, 16); BatSample fl; memcpy(&fl, b + sz - 16, 16); free(b);
  OK((f0.spare & LOGF_COLD) && !(fl.spare & LOGF_COLD) && fl.asks == hw.batPct && fl.mv == hw.batMv && fl.tin10 == 214, "the first marked as a power-on start; the last holds the cell, its percentage and the room");
  // USB in: charging, topping up, full
  hw.usb = true; hw.chg = true; hw.chgState = 1; hw.batMv = 3990; hw.batPct = 80; quiet(3600);
  hw.chgState = 2; hw.batMv = 4200; hw.batPct = 100; quiet(5400);
  hw.sceneEdit = true; R.scr = SCR_TILE; R.tile = T_DEVICE; dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_UP } }); snap("48-device-topping-up");
  OK(has(fieldLog(), "USB, topping up"), "the field log says topping up");
  hw.chgState = 3; hw.chg = false; quiet(3600);
  hw.usb = false; hw.chgState = 0; hw.batMv = 4150; hw.batPct = 99;
  for (int i = 0; i < 20; i++) { hw.batMv -= 4; if (i % 3 == 0) hw.batPct -= 1; quiet(1800); }
  hw.sceneEdit = true; R.scr = SCR_LIST; R.list = L_DEVICE; R.cur = 5; R.tile = T_DEVICE; dialWake(PIN_DOWN, 120, { { 2000, 120, PIN_UP }, { 4000, 120, PIN_PRESS } }); snap("49-battery-page");
  OK(R.scr == SCR_BAT && marginsClear(), "Device > Battery: the page fits the paper");
  b = NULL; sz = fsRead("/log.bin", &b); int n = sz / 16, topup = 0, full = 0, usb = 0;
  for (int i = 0; i < n; i++) { BatSample s; memcpy(&s, b + i * 16, 16); if (s.spare & LOGF_TOPUP) topup++; if (s.spare & LOGF_FULL) full++; if (s.spare & LOGF_USB) usb++; }
  OK(topup >= 2 && full >= 1 && usb >= topup + full, "charging, topping up and full are each in the log");
  BatSample s; memcpy(&s, b, 16); free(b);
  char line[160]; logCsvLine(s, line, sizeof line);
  OK(!strncmp(line, "2026-10-26T", 11) && strstr(line, ",4100,0,95,21.4,47,,,1\n"), "a CSV line: time, millivolts, Wi-Fi, percentage, room, the flags");
  // awake for a long time (reading with the keyboard): the sample still comes, marked awake
  newDevice("battery-awake"); hw.sdThere = true; shelfCard(); hostPrefs["kbA"] = "be0000000000"; hostPrefs["kbT"] = "1"; hw.kbThere = true;
  powerOn();
  hw.sceneEdit = true; R.scr = SCR_TILE; R.tile = T_SHELF; R.lastSample = nowS() - 1700;
  std::vector<K> ks; for (int i = 0; i < 12; i++) ks.push_back({ 0, (uint8_t)(i % 2 ? 0x51 : 0x52) });
  keysAt(5000, ks, 15000);                                                 // a key every 15 s for three minutes
  size_t before = fsSize("/log.bin");
  dialWake(PIN_PRESS, 120, { { 2000, 120, PIN_PRESS } });
  b = NULL; sz = fsRead("/log.bin", &b); memcpy(&s, b + sz - 16, 16); free(b);
  OK(sz == before + 16 && (s.spare & LOGF_AWAKE) && (s.spare & LOGF_KB), "awake with the keyboard: the 30-minute sample still comes, marked awake and keyboard");
}


// ---------------------------------------------------------------- v0.4.0: the keyboard everywhere
static int countOf(const std::string &t, const char *x) { int n = 0; for (size_t i = t.find(x); i != std::string::npos; i = t.find(x, i + 1)) n++; return n; }
#define ESC4 { 0, 0x29 }, { 0, 0x29 }, { 0, 0x29 }, { 0, 0x29 }
static void sceneKeyboard() {
  printf("the keyboard everywhere: tiles, lists, cards, codes, the menu, the typewriter's Esc, the archives, pairing from the menu\n");
  newDevice("keys"); hw.sdThere = true; shelfCard(); hostPrefs["kbA"] = "be0000000000"; hostPrefs["kbT"] = "1"; hw.kbThere = true; hw.kbBat = 70;
  // switched on: the keyboard is listened for and comes back; → → Enter Enter Enter Enter space Esc: Editions, a pack, a section, a card, the next, back
  keysAt(6000, { { 0, 0x4F }, { 0, 0x4F }, { 0, 0x28 }, { 0, 0x28 }, { 0, 0x28 }, { 0, 0x28 }, { 0, 0x2C }, { 0, 0x29 } }, 900);
  powerOn();
  OK(has(fieldLog(), "[keyboard] back, no new pairing") && R.scr == SCR_LIST && R.list == L_CARDS && hw.badPartials == 0, "switched on, the keyboard comes back; arrows and Enter go into Editions, space turns, Esc goes back to the cards");
  // 7 Enter b Enter: Device, its menu, the letter b jumps to Battery, Enter opens it
  keysAt(4000, { ESC4, { 0, 0x24 }, { 0, 0x28 }, { 0, 0x05 }, { 0, 0x28 } }, 900);
  dialWake(PIN_UP); snap("50-keys-battery");
  OK(R.scr == SCR_BAT && marginsClear(), "Esc to the Clock, 7, Enter, b, Enter: the battery page, from the keyboard alone");
  // a dial wake on the battery page: back to the menu; End, Esc, 1: the last row, the tile, the Clock
  keysAt(4000, { { 0, 0x4D }, { 0, 0x29 }, { 0, 0x1E } }, 900);
  int backs = countOf(fieldLog(), "[keyboard] back");
  dialWake(PIN_UP);
  OK(R.scr == SCR_TILE && R.tile == T_CLOCK && countOf(fieldLog(), "[keyboard] back") == backs + 1, "a dial wake: the keyboard listened for and back; End, Esc, 1: the Clock");
  // 6 Enter Enter →: the Catalog, its list, a code, the next code
  keysAt(4000, { ESC4, { 0, 0x23 }, { 0, 0x28 }, { 0, 0x28 }, { 0, 0x4F } }, 900);
  dialWake(PIN_UP); snap("51-keys-code");
  OK(R.scr == SCR_QR && R.qr == 1, "Esc to the Clock, 6, Enter, Enter, \xE2\x86\x92: the Catalog's second code");
  // 4 Enter, type ok, Enter, Esc, 1: the typewriter, a saved line, Esc leaves it, and the keys still drive the tiles
  std::vector<K> ks = { ESC4, { 0, 0x21 }, { 0, 0x28 } }; auto t = typeStr("ok"); ks.insert(ks.end(), t.begin(), t.end());
  ks.push_back({ 0, 0x28 }); ks.push_back({ 0, 0x29 }); ks.push_back({ 0, 0x1E });
  keysAt(4000, ks, 1500);
  dialWake(PIN_UP);
  OK(sdFile(String(twPath().c_str() + 7).c_str()).find("ok\n") != std::string::npos && R.scr == SCR_TILE && R.tile == T_CLOCK && rot == 0, "Esc to the Clock, 4, Enter, ok, Enter, Esc, 1: the typewriter saves the line, Esc leaves it, 1 goes to the Clock");
  // 5 Enter Enter, berlin, Enter: the Archives, the first archive, the search, the article
  ks = { ESC4, { 0, 0x22 }, { 0, 0x28 }, { 0, 0x28 } }; t = typeStr("berlin"); ks.insert(ks.end(), t.begin(), t.end()); ks.push_back({ 0, 0x28 });
  keysAt(4000, ks, 900);
  dialWake(PIN_UP); snap("52-keys-archive");
  OK(R.scr == SCR_READ && rdLoad() && rdTitle == "Berlin" && hw.badPartials == 0, "Esc to the Clock, 5, Enter, Enter, berlin, Enter: Berlin, read; every partial refresh from the paper");
  // the menu: forget the keyboard with the keys, then pair it again from the same row with the dial
  keysAt(4000, { ESC4, { 0, 0x24 }, { 0, 0x28 }, { 0, 0x0E }, { 0, 0x28 } }, 900);
  dialWake(PIN_UP); snap("53-keys-forgot");
  OK(!hostPrefs.count("kbA") && has(fieldLog(), "[keyboard] forgotten") && R.scr == SCR_LIST && R.cur == 3, "Esc to the Clock, 7, Enter, k, Enter: the keyboard forgotten from the menu");
  dialWake(PIN_PRESS, 120, { { 3000, 120, PIN_PRESS } }); snap("54-menu-paired");
  OK(hostPrefs.count("kbA") && countOf(fieldLog(), "[keyboard] paired and bonded") == 1 && R.scr == SCR_LIST && R.list == L_DEVICE, "the same row now says pair one: a press lists the keyboards, a press pairs it");
  // timer wakes never listen
  backs = countOf(fieldLog(), "[keyboard] back");
  quiet(3 * 3600);
  OK(countOf(fieldLog(), "[keyboard] back") == backs && !hw.kbConnected, "three hours of timer wakes: the keyboard never listened for");
}

int main(int argc, char **argv) {
  mainPid = getpid();
  if (argc > 1) outDir = argv[1];
  std::string cmd = "mkdir -p " + outDir; if (system(cmd.c_str())) {}
  setvbuf(stdout, NULL, _IONBF, 0);
  sceneFirstLight();
  sceneDial();
  sceneCards();
  sceneCatalogDevice();
  sceneSetupScreen();
  sceneNoClock();
  sceneBattery();
  sceneTypewriter();
  sceneShelf();
  sceneBatLog();
  sceneKeyboard();
  printf("397 bench: %d checks, %d failed, %d wakes run\n", checks, fails, wakesRun);
  return fails ? 1 : 0;
}
