// ek_shared.h: the parts every E!nkster board shares.
// Tile contract v1, 12 October 2026. Owner: the firmware chat.
//
// One header, no library beyond the ESP32 Arduino core. The pure parts (journal line,
// recipe, tile fit, Wi-Fi order) also compile on a computer, for the bench.
//
//   1. Settings names     EK_SET_*
//   2. Journal line       ekJournalLine(), ekJournalHeader(), ekJournalHide()
//   3. Wi-Fi list         EkNets (stored in the settings, up to EK_NETS_MAX networks)
//   4. Recipe             ekRecipeParse(), ekRecipeFromLink(), ekRecipeToJson()
//   5. Tile declarations  EkTileDecl, EkBoardDecl, ekTileFits()
//
// Rules that hold for all of it:
//   - No key, no network name, no password, no place ever goes into a journal line.
//   - A recipe is the only thing of a device that is ever published.
//   - Nothing personal is compiled in. Everything personal lives in the settings.
//
// Written by Julien and Claude the AI
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define EK_SHARED_VERSION "1.0.3"   // 1.0.3: the tile ids marbleman and thepushbutt0n, the height setting, the motion need (contract v17)
                                    // 1.0.2: the clicker tile id and the room sensor correction (contract v9)
                                    // 1.0.1: the Preferences adapter also builds wherever <Preferences.h> exists (the simulator)

// ======================================================================== 1. settings names
// One namespace for every board: "einkster". Keys are 15 characters at most (the NVS limit).
// A setting that exists on two boards has one name. A board-only setting starts with the
// board's short prefix: "fr" for the frames, "x4" for the X4.
#define EK_SET_NAMESPACE  "einkster"
#define EK_SET_LANG       "lang"     // interface language: "en", "fr", "de", "ja", "ru"
#define EK_SET_TZ         "tz"       // POSIX time zone; default EK_TZ_DEFAULT. The clock chip keeps world time.
#define EK_SET_RECIPE     "recipe"   // the recipe, as JSON (section 4)
#define EK_SET_NET_COUNT  "wN"       // number of stored networks
#define EK_SET_NET_NAME   "wS"       // + index 0..4: network name
#define EK_SET_NET_PASS   "wP"       // + index 0..4: password
#define EK_SET_NET_LAST   "net"      // index of the network that worked last
#define EK_SET_TOWN       "town"     // display name of the place. Personal: never in the journal.
#define EK_SET_LAT        "lat"      // decimal degrees as text, e.g. "48.75". Personal.
#define EK_SET_LON        "lon"      // personal
#define EK_SET_KEY_CLAUDE "kClaude"  // the owner's own keys. Never shown again once stored, never logged.
#define EK_SET_KEY_DG     "kDeepgram"
#define EK_SET_HOTSPOT_PW "apPw"     // the setup hotspot's password, made once per device
#define EK_SET_TEMP_OFF  "tOff"     // room sensor correction, tenths of a degree, added to the reading
#define EK_SET_ALT        "alt"      // the device's height above sea level, whole metres (an int), for a pressure sensor's
                                    // sea-level value. Personal like lat and lon: never in the journal. Unset: the board's own figure.
#define EK_TEMP_OFF_DEFAULT (-10)   // -1.0: the 154's case warms its sensor by about a degree (checked 14.10.2026)
#define EK_SET_JOURNAL_ON "flOn"     // journal switch; on by default on test units only
#define EK_SET_JOURNAL_RUN  "flRun"  // journal run counter (kept from v0.23)
#define EK_SET_JOURNAL_OPEN "flOpen" // a run is open: the last one did not end cleanly
#define EK_SET_JOURNAL_DROP "flDrop" // lines dropped when the older file was rotated out
// Old school-build keys, read once and moved into the list (EkNets::load does it):
#define EK_SET_OLD_SSID   "wSsid"
#define EK_SET_OLD_PASS   "wPass"

#define EK_TZ_DEFAULT "CET-1CEST,M3.5.0,M10.5.0/3"   // Central Europe with summer time

// ======================================================================== 2. journal line
// One line per event, plain text, at most EK_JOURNAL_MAX bytes:
//
//   T+MM:SS.t  HH:MM:SS.mmm  [type] detail
//
//   T+        time since the run began (tenths of a second); minutes run past 59
//   HH:MM:SS  local wall clock, or "--:--:--.---" when the clock is not set
//   type      one word from the list below; a board may add its own words
//   detail    free text, never a secret (ekJournalHide() blanks any that slip in)
//
// Files: /field.txt, rotated to /field.old at 40 kB. A run starts with ekJournalHeader().
// Shared types: wake, sleep, wifi, time, fetch, battery, tile, log, MARK, end, sensor, draw, start.
#define EK_JOURNAL_MAX 220

static inline size_t ekJournalLine(char *out, size_t n, uint32_t tenths, const struct tm *local, int ms,
                                   const char *type, const char *detail) {
  char clk[16];
  if (local) snprintf(clk, sizeof clk, "%02d:%02d:%02d.%03d", local->tm_hour, local->tm_min, local->tm_sec, ms % 1000);
  else strcpy(clk, "--:--:--.---");
  if (n > EK_JOURNAL_MAX + 1) n = EK_JOURNAL_MAX + 1;
  int k = snprintf(out, n, "T+%02lu:%02lu.%lu  %s  [%s] %s", (unsigned long)(tenths / 600),
                   (unsigned long)(tenths / 10 % 60), (unsigned long)(tenths % 10), clk, type, detail ? detail : "");
  if (k < 0) { out[0] = 0; return 0; }
  return (size_t)k < n ? (size_t)k : n - 1;
}

// The header block of a run. 'emit' receives each line; a blank line comes first.
// role: "classic", "school", "frame", ... ; reset: why the chip started ("power on", "deep sleep", ...).
typedef void (*EkLineSink)(const char *line, void *ctx);
static inline void ekJournalHeader(EkLineSink emit, void *ctx, const char *app, const char *version, const char *role,
                                   const char *deviceName, uint32_t run, const struct tm *started, const char *reset,
                                   int batteryMv) {
  char b[96];
  emit("", ctx);
  emit("FIELD LOG", ctx);
  snprintf(b, sizeof b, "app       %s", app); emit(b, ctx);
  snprintf(b, sizeof b, "version   %s", version); emit(b, ctx);
  snprintf(b, sizeof b, "role      %s", role); emit(b, ctx);
  snprintf(b, sizeof b, "device    %s", deviceName); emit(b, ctx);
  snprintf(b, sizeof b, "run       %lu", (unsigned long)run); emit(b, ctx);
  if (started) strftime(b, sizeof b, "started   %Y-%m-%d %H:%M:%S", started); else strcpy(b, "started   clock not set");
  emit(b, ctx);
  snprintf(b, sizeof b, "reset     %s", reset); emit(b, ctx);
  if (batteryMv >= 2800) snprintf(b, sizeof b, "battery   %d.%02d V", batteryMv / 1000, batteryMv % 1000 / 10);
  else strcpy(b, "battery   no cell seen");
  emit(b, ctx);
}

// Blanks every stored secret found in a line, in place. 'secrets' ends with NULL.
// Values shorter than 4 characters are skipped (they would blank ordinary words).
// Returns how many were blanked. Call it on every line before it is written.
static inline int ekJournalHide(char *line, const char *const *secrets) {
  int hits = 0;
  for (; secrets && *secrets; secrets++) {
    size_t m = strlen(*secrets);
    if (m < 4) continue;
    char *p;
    while ((p = strstr(line, *secrets)) != NULL) {
      const char *rep = "[hidden]"; size_t r = 8;
      memmove(p + r, p + m, strlen(p + m) + 1);
      memcpy(p, rep, r); hits++;
    }
  }
  return hits;
}

// ======================================================================== 3. Wi-Fi list
// Up to EK_NETS_MAX networks in the settings. Tried in this order: the one that worked last,
// then the others in the order they were added. Adding a network that is already stored
// replaces its password. Adding one more than the room drops the oldest that is not the last good one.
// The journal names a network by its position only: "network 2 of 3".
#define EK_NETS_MAX 5
#define EK_NET_NAME_MAX 32
#define EK_NET_PASS_MAX 64

// The settings store, as the shared code sees it. EkPrefsKV below adapts the core's Preferences.
struct EkKV {
  virtual size_t getStr(const char *key, char *out, size_t n) = 0;   // "" when absent
  virtual void putStr(const char *key, const char *v) = 0;
  virtual int getInt(const char *key, int def) = 0;
  virtual void putInt(const char *key, int v) = 0;
  virtual void remove(const char *key) = 0;
  virtual ~EkKV() {}
};

struct EkNets {
  char name[EK_NETS_MAX][EK_NET_NAME_MAX + 1];
  char pass[EK_NETS_MAX][EK_NET_PASS_MAX + 1];
  int count = 0;
  int last = 0;

  static void key(char *k, const char *base, int i) { snprintf(k, 8, "%s%d", base, i); }

  void load(EkKV &kv) {
    count = kv.getInt(EK_SET_NET_COUNT, -1);
    if (count < 0) {                            // never stored in the list form: take the old school keys once
      count = 0;
      char s[EK_NET_NAME_MAX + 1], p[EK_NET_PASS_MAX + 1];
      kv.getStr(EK_SET_OLD_SSID, s, sizeof s); kv.getStr(EK_SET_OLD_PASS, p, sizeof p);
      if (s[0]) { add(kv, s, p); kv.remove(EK_SET_OLD_SSID); kv.remove(EK_SET_OLD_PASS); }
      else kv.putInt(EK_SET_NET_COUNT, 0);
      return;
    }
    if (count > EK_NETS_MAX) count = EK_NETS_MAX;
    char k[8];
    for (int i = 0; i < count; i++) {
      key(k, EK_SET_NET_NAME, i); kv.getStr(k, name[i], sizeof name[i]);
      key(k, EK_SET_NET_PASS, i); kv.getStr(k, pass[i], sizeof pass[i]);
    }
    last = kv.getInt(EK_SET_NET_LAST, 0);
    if (last < 0 || last >= count) last = 0;
  }
  void save(EkKV &kv) {
    char k[8];
    for (int i = 0; i < EK_NETS_MAX; i++) {
      key(k, EK_SET_NET_NAME, i); if (i < count) kv.putStr(k, name[i]); else kv.remove(k);
      key(k, EK_SET_NET_PASS, i); if (i < count) kv.putStr(k, pass[i]); else kv.remove(k);
    }
    kv.putInt(EK_SET_NET_COUNT, count);
    kv.putInt(EK_SET_NET_LAST, last);
  }
  int find(const char *n) const { for (int i = 0; i < count; i++) if (!strcmp(name[i], n)) return i; return -1; }
  // false: empty name, or a name or password too long
  bool add(EkKV &kv, const char *n, const char *p) {
    if (!n || !n[0] || strlen(n) > EK_NET_NAME_MAX || strlen(p ? p : "") > EK_NET_PASS_MAX) return false;
    int i = find(n);
    if (i < 0) {
      if (count == EK_NETS_MAX) {               // full: drop the oldest that is not the last good one
        int drop = (last == 0) ? 1 : 0;
        dropAt(drop);
      }
      i = count++;
      strcpy(name[i], n);
    }
    strcpy(pass[i], p ? p : "");
    save(kv);
    return true;
  }
  bool removeAt(EkKV &kv, int i) {
    if (i < 0 || i >= count) return false;
    dropAt(i); save(kv); return true;
  }
  void dropAt(int i) {
    for (int j = i; j + 1 < count; j++) { strcpy(name[j], name[j + 1]); strcpy(pass[j], pass[j + 1]); }
    count--;
    if (last == i) last = 0; else if (last > i) last--;
  }
  // k-th network to try, k = 0..count-1
  int order(int k) const {
    if (!count) return -1;
    if (k == 0) return last;
    int n = 0;
    for (int i = 0; i < count; i++) { if (i == last) continue; if (++n == k) return i; }
    return -1;
  }
  void worked(EkKV &kv, int i) { if (i >= 0 && i < count && i != last) { last = i; kv.putInt(EK_SET_NET_LAST, i); } }
};

// ======================================================================== 4. recipe
// {
//   "einkster_recipe": 1,
//   "name": "classic",              a-z, 0-9 and '-', 1 to 24 characters; drawn on the device tile
//   "board": "154",                 154, 397, x4, pebble, frame-colour, frame-bw
//   "base": "0.27.0",               the firmware the recipe was made on
//   "tiles": ["ask", "clock"],      1 to 12, each once; the device tile is never listed
//   "editions": ["catlas"]          0 to 12 Edition family names; never a language code
// }
// An id the device does not know is skipped, and listed in 'skipped'. A recipe with no tile
// the device knows is refused. Unknown fields are ignored, so an old device takes a newer recipe.
// Link form (setup page, hotspot only):  /r?n=classic&b=154&v=0.27.0&t=ask,clock&e=catlas
#define EK_RECIPE_MAX_TILES 12
#define EK_RECIPE_MAX_EDS   12
#define EK_ID_MAX           24

enum EkRecipeErr { EK_RECIPE_OK = 0, EK_RECIPE_SYNTAX, EK_RECIPE_FORMAT, EK_RECIPE_NAME, EK_RECIPE_NO_TILE,
                   EK_RECIPE_DUPLICATE, EK_RECIPE_TOO_MANY };

struct EkRecipe {
  int format = 0;
  char name[EK_ID_MAX + 1] = "";
  char board[16] = "";
  char base[16] = "";
  char tiles[EK_RECIPE_MAX_TILES][EK_ID_MAX + 1];
  int nTiles = 0;
  char editions[EK_RECIPE_MAX_EDS][EK_ID_MAX + 1];
  int nEds = 0;
  char skipped[8][EK_ID_MAX + 1];                // tile ids the device did not know (first 8)
  int nSkipped = 0;
};

typedef bool (*EkKnowsTile)(const char *id);

static inline bool ekIdOk(const char *s) {
  size_t n = strlen(s);
  if (!n || n > EK_ID_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

// --- a small reader for this one shape of JSON
struct EkJ {
  const char *p, *end;
  void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
  bool lit(char c) { ws(); if (p < end && *p == c) { p++; return true; } return false; }
  // a string into out (n bytes); escapes kept simple: \" \\ \/ \n \t, \u for ASCII only
  bool str(char *out, size_t n) {
    ws(); if (p >= end || *p != '"') return false; p++;
    size_t k = 0;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c == '\\') {
        if (p >= end) return false;
        char e = *p++;
        if (e == 'n') c = '\n'; else if (e == 't') c = '\t';
        else if (e == 'u') {
          if (end - p < 4) return false;
          unsigned v = 0;
          for (int i = 0; i < 4; i++) { char h = *p++; v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
          c = v < 128 ? (char)v : '?';
        } else c = e;
      }
      if (out && k + 1 < n) out[k++] = c;
    }
    if (p >= end) return false;
    p++; if (out && n) out[k] = 0;
    return true;
  }
  bool num(long *v) {
    ws(); const char *s = p; bool neg = false; long x = 0;
    if (p < end && *p == '-') { neg = true; p++; }
    while (p < end && *p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
    while (p < end && (*p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-' || (*p >= '0' && *p <= '9'))) p++;
    if (p == s) return false;
    if (v) *v = neg ? -x : x;
    return true;
  }
  bool skip() {                                   // any value
    ws(); if (p >= end) return false;
    if (*p == '"') return str(NULL, 0);
    if (*p == '{' || *p == '[') {
      char open = *p, close = open == '{' ? '}' : ']'; p++;
      if (lit(close)) return true;
      do {
        if (open == '{') { if (!str(NULL, 0) || !lit(':')) return false; }
        if (!skip()) return false;
      } while (lit(','));
      return lit(close);
    }
    if (!strncmp(p, "true", 4)) { p += 4; return true; }
    if (!strncmp(p, "false", 5)) { p += 5; return true; }
    if (!strncmp(p, "null", 4)) { p += 4; return true; }
    return num(NULL);
  }
};

static inline int ekRecipeAddTile(EkRecipe &r, const char *id, EkKnowsTile knows) {
  if (!ekIdOk(id) || !strcmp(id, "device")) return EK_RECIPE_OK;          // the device tile is fixed: ignore it
  if (knows && !knows(id)) { if (r.nSkipped < 8) strcpy(r.skipped[r.nSkipped++], id); return EK_RECIPE_OK; }
  for (int i = 0; i < r.nTiles; i++) if (!strcmp(r.tiles[i], id)) return EK_RECIPE_DUPLICATE;
  if (r.nTiles == EK_RECIPE_MAX_TILES) return EK_RECIPE_TOO_MANY;
  strcpy(r.tiles[r.nTiles++], id);
  return EK_RECIPE_OK;
}
static inline int ekRecipeAddEd(EkRecipe &r, const char *id) {
  if (!ekIdOk(id)) return EK_RECIPE_OK;
  for (int i = 0; i < r.nEds; i++) if (!strcmp(r.editions[i], id)) return EK_RECIPE_OK;   // twice: once is enough
  if (r.nEds == EK_RECIPE_MAX_EDS) return EK_RECIPE_TOO_MANY;
  strcpy(r.editions[r.nEds++], id);
  return EK_RECIPE_OK;
}
static inline int ekRecipeCheck(EkRecipe &r) {
  if (r.format != 1) return EK_RECIPE_FORMAT;
  if (!ekIdOk(r.name)) return EK_RECIPE_NAME;
  if (!r.nTiles) return EK_RECIPE_NO_TILE;
  return EK_RECIPE_OK;
}

static inline int ekRecipeParse(const char *json, size_t len, EkRecipe &r, EkKnowsTile knows) {
  r = EkRecipe();
  EkJ j{json, json + len};
  if (!j.lit('{')) return EK_RECIPE_SYNTAX;
  if (j.lit('}')) return ekRecipeCheck(r);
  do {
    char k[32];
    if (!j.str(k, sizeof k) || !j.lit(':')) return EK_RECIPE_SYNTAX;
    if (!strcmp(k, "einkster_recipe")) { long v; if (!j.num(&v)) return EK_RECIPE_SYNTAX; r.format = (int)v; }
    else if (!strcmp(k, "name"))  { if (!j.str(r.name, sizeof r.name)) return EK_RECIPE_SYNTAX; }
    else if (!strcmp(k, "board")) { if (!j.str(r.board, sizeof r.board)) return EK_RECIPE_SYNTAX; }
    else if (!strcmp(k, "base"))  { if (!j.str(r.base, sizeof r.base)) return EK_RECIPE_SYNTAX; }
    else if (!strcmp(k, "tiles") || !strcmp(k, "editions")) {
      bool t = k[0] == 't';
      if (!j.lit('[')) return EK_RECIPE_SYNTAX;
      if (!j.lit(']')) {
        do {
          char id[64]; if (!j.str(id, sizeof id)) return EK_RECIPE_SYNTAX;
          int e = t ? ekRecipeAddTile(r, id, knows) : ekRecipeAddEd(r, id);
          if (e) return e;
        } while (j.lit(','));
        if (!j.lit(']')) return EK_RECIPE_SYNTAX;
      }
    }
    else if (!j.skip()) return EK_RECIPE_SYNTAX;
  } while (j.lit(','));
  if (!j.lit('}')) return EK_RECIPE_SYNTAX;
  return ekRecipeCheck(r);
}

// The query part of a link, after '?'. Percent escapes are not needed: every value is [a-z0-9-.,].
static inline int ekRecipeFromLink(const char *q, EkRecipe &r, EkKnowsTile knows) {
  r = EkRecipe(); r.format = 1;
  while (*q) {
    const char *amp = strchr(q, '&'); size_t n = amp ? (size_t)(amp - q) : strlen(q);
    if (n > 2 && q[1] == '=') {
      char v[400]; size_t m = n - 2 < sizeof v - 1 ? n - 2 : sizeof v - 1;
      memcpy(v, q + 2, m); v[m] = 0;
      char c = q[0];
      if (c == 'n') { strncpy(r.name, v, EK_ID_MAX); r.name[EK_ID_MAX] = 0; }
      else if (c == 'b') { strncpy(r.board, v, 15); r.board[15] = 0; }
      else if (c == 'v') { strncpy(r.base, v, 15); r.base[15] = 0; }
      else if (c == 't' || c == 'e') {
        char *s = v;
        while (s && *s) {
          char *comma = strchr(s, ','); if (comma) *comma = 0;
          int e = c == 't' ? ekRecipeAddTile(r, s, knows) : ekRecipeAddEd(r, s);
          if (e) return e;
          s = comma ? comma + 1 : NULL;
        }
      }
    }
    q += n; if (*q == '&') q++;
  }
  return ekRecipeCheck(r);
}

static inline size_t ekRecipeToJson(const EkRecipe &r, char *out, size_t n) {
  size_t k = snprintf(out, n, "{\"einkster_recipe\":1,\"name\":\"%s\",\"board\":\"%s\",\"base\":\"%s\",\"tiles\":[",
                      r.name, r.board, r.base);
  for (int i = 0; i < r.nTiles && k < n; i++) k += snprintf(out + k, n - k, "%s\"%s\"", i ? "," : "", r.tiles[i]);
  if (k < n) k += snprintf(out + k, n - k, "],\"editions\":[");
  for (int i = 0; i < r.nEds && k < n; i++) k += snprintf(out + k, n - k, "%s\"%s\"", i ? "," : "", r.editions[i]);
  if (k < n) k += snprintf(out + k, n - k, "]}");
  return k < n ? k : n - 1;
}

static inline const char *ekRecipeErrText(int e) {
  switch (e) {
    case EK_RECIPE_OK: return "ok";
    case EK_RECIPE_SYNTAX: return "not a recipe";
    case EK_RECIPE_FORMAT: return "unknown recipe format";
    case EK_RECIPE_NAME: return "name: a-z, 0-9 and -, 24 characters at most";
    case EK_RECIPE_NO_TILE: return "no tile this device knows";
    case EK_RECIPE_DUPLICATE: return "a tile is listed twice";
    case EK_RECIPE_TOO_MANY: return "more than 12";
    default: return "error";
  }
}

// ======================================================================== 5. tile declarations
// A tile declares what it needs and how long its face stays true. The picker shows a tile on a
// board only when the board has everything the tile needs and redraws fast enough for it.
// A tile draws inside the box it is given, and must be able to draw in black and white.
enum EkNeed : uint16_t {
  EK_NEED_NET   = 1 << 0,   // Wi-Fi
  EK_NEED_KEY   = 1 << 1,   // the owner's own key (onboarding step 3)
  EK_NEED_CLOCK = 1 << 2,   // a set clock
  EK_NEED_CARD  = 1 << 3,   // a memory card
  EK_NEED_MIC   = 1 << 4,
  EK_NEED_SOUND = 1 << 5,   // a speaker
  EK_NEED_INPUT = 1 << 6,   // touch, knob or buttons for more than a page turn
  EK_NEED_ROOM  = 1 << 7,   // a room sensor
  EK_NEED_MOTION = 1 << 8,  // a motion chip (tilt): 1.0.3
};
enum EkColour : uint8_t { EK_BW_ONLY = 0, EK_CAN_COLOUR = 1, EK_NEEDS_COLOUR = 2 };

struct EkTileDecl {
  const char *id;
  uint32_t trueFor_s;     // how long a drawn face stays true; 0 = only while live
  uint16_t needs;         // EkNeed bits
  uint8_t colour;         // EkColour
  uint8_t state;          // 0 frozen id, 1 proposed (Julien to rule), 2 test tool, 3 retired
};
struct EkBoardDecl {
  const char *id;
  uint16_t w, h;
  uint32_t redraw_s;      // the board's own rhythm: 0 live, 60 minutes, 86400 once a day
  uint16_t has;           // EkNeed bits the board can give
  uint8_t colour;         // EK_BW_ONLY or EK_CAN_COLOUR
  bool deviceTile;        // carries the fixed device tile (boards with an input)
};
struct EkBox { int16_t x, y, w, h; };

// A face that stays true for t seconds fits a board that redraws every r seconds when t >= r.
// A tile can make itself fit by printing the time of its data ("reading of 6:00").
static inline bool ekTileFits(const EkTileDecl &t, const EkBoardDecl &b) {
  if (t.state == 3) return false;
  if ((t.needs & b.has) != t.needs) return false;
  if (t.colour == EK_NEEDS_COLOUR && b.colour != EK_CAN_COLOUR) return false;
  if (b.redraw_s && (t.trueFor_s == 0 || t.trueFor_s < b.redraw_s)) return false;
  return true;
}

static const EkTileDecl EK_TILES[] = {
  // id           true for  needs                                                      colour        state
  { "ask",        0,        EK_NEED_NET | EK_NEED_KEY | EK_NEED_MIC | EK_NEED_INPUT,    EK_BW_ONLY,   0 },
  { "derdiedas",  0,        EK_NEED_NET | EK_NEED_KEY | EK_NEED_MIC | EK_NEED_INPUT,    EK_BW_ONLY,   0 },
  { "oracle",     86400,    EK_NEED_NET,                                                EK_BW_ONLY,   0 },   // no network once it reads a pack (open)
  { "jar",        86400,    EK_NEED_NET | EK_NEED_INPUT,                                EK_BW_ONLY,   0 },
  { "kaffeesatz", 86400,    EK_NEED_NET,                                                EK_BW_ONLY,   0 },
  { "radio",      0,        EK_NEED_NET | EK_NEED_SOUND | EK_NEED_INPUT,                EK_BW_ONLY,   0 },
  { "climate",    1800,     EK_NEED_ROOM,                                               EK_CAN_COLOUR,0 },
  { "clock",      60,       EK_NEED_CLOCK,                                              EK_BW_ONLY,   0 },
  { "editions",   0,        EK_NEED_INPUT,                                              EK_CAN_COLOUR,0 },
  { "catalog",    0,        EK_NEED_INPUT,                                              EK_BW_ONLY,   0 },
  { "clicker",    0,        EK_NEED_INPUT,                                              EK_BW_ONLY,   0 },   // contract v9: holds the talk
  { "marbleman",  0,        EK_NEED_INPUT | EK_NEED_MOTION | EK_NEED_SOUND,             EK_CAN_COLOUR,0 },   // contract v17: the 175's tilt game
  { "thepushbutt0n", 0,     EK_NEED_INPUT,                                              EK_CAN_COLOUR,0 },   // contract v17: the 175 as a one-key Bluetooth keyboard for the app
  { "battery",    1800,     0,                                                          EK_BW_ONLY,   2 },
  { "weather",    0,        0,                                                          EK_BW_ONLY,   3 },
  // frame ids: proposed in the frame brief, open in the contract
  { "picture",    86400,    EK_NEED_CARD,                                               EK_CAN_COLOUR,1 },
  { "date",       86400,    EK_NEED_CLOCK,                                              EK_BW_ONLY,   1 },
  { "forecast",   86400,    EK_NEED_NET,                                                EK_CAN_COLOUR,1 },
  { "room",       86400,    EK_NEED_ROOM,                                               EK_BW_ONLY,   1 },
  { "caption",    86400,    0,                                                          EK_BW_ONLY,   1 },
};
static const int EK_TILE_COUNT = sizeof EK_TILES / sizeof EK_TILES[0];
// "device" is fixed and in no recipe; "notes" is kept for the audio note tile.

static const EkBoardDecl EK_BOARDS[] = {
  // id             w    h    redraw  has                                                                         colour         device tile
  { "154",          200, 200, 0,      0x00FF,                                                                     EK_BW_ONLY,    true  },
  { "397",          800, 480, 0,      0x00FF,                                                                     EK_BW_ONLY,    true  },
  { "pebble",       466, 466, 0,      (0x00FF & ~(EK_NEED_CARD | EK_NEED_ROOM)) | EK_NEED_MOTION,                 EK_CAN_COLOUR, true  },   // no card slot (schematic, 19.10); motion chip QMI8658
  { "frame-colour", 800, 480, 86400,  EK_NEED_NET | EK_NEED_KEY | EK_NEED_CLOCK | EK_NEED_CARD | EK_NEED_ROOM | EK_NEED_MIC | EK_NEED_SOUND, EK_CAN_COLOUR, false },
};
static const int EK_BOARD_COUNT = sizeof EK_BOARDS / sizeof EK_BOARDS[0];

static inline const EkTileDecl *ekTile(const char *id) {
  for (int i = 0; i < EK_TILE_COUNT; i++) if (!strcmp(EK_TILES[i].id, id)) return &EK_TILES[i];
  return NULL;
}
static inline const EkBoardDecl *ekBoard(const char *id) {
  for (int i = 0; i < EK_BOARD_COUNT; i++) if (!strcmp(EK_BOARDS[i].id, id)) return &EK_BOARDS[i];
  return NULL;
}

// ======================================================================== the core's Preferences
#if defined(ARDUINO) || (defined(__has_include) && __has_include(<Preferences.h>))
#include <Preferences.h>
struct EkPrefsKV : EkKV {
  Preferences &p;
  explicit EkPrefsKV(Preferences &pr) : p(pr) {}
  size_t getStr(const char *k, char *out, size_t n) override {
    if (!n) return 0;
    out[0] = 0;
    if (!p.isKey(k)) return 0;
    return p.getString(k, out, n);
  }
  void putStr(const char *k, const char *v) override { p.putString(k, v); }
  int getInt(const char *k, int d) override { return p.getInt(k, d); }
  void putInt(const char *k, int v) override { p.putInt(k, v); }
  void remove(const char *k) override { if (p.isKey(k)) p.remove(k); }
};
#endif
