// ek_link.h: the radio link between the E!nkster 154 (the Cl!cker tile, cat toy mode)
// and the peek-a-boo box. Owner: the firmware chat. The cat toy chat proposes changes.
// Version 1.1.2, 15 October 2026. 1.1.2: words only, the budget in every answer.
// 1.1.1: words only, the hand budget (Julien, 15.10).
// 1.1.0: mode 4 "waiting" and the "no answer" sentence.
//
// Both firmwares compile this same file, so the two sides cannot drift.
// Pure C++ with no library: the packet, the codes, the timings, and three small helpers
// (encode and decode, the sequence filter, the heartbeat watch). The radio calls
// themselves (ESP-NOW) stay in each firmware.
//
// What it rests on: ESP-NOW, Espressif's direct radio between ESP32 boards, in the
// ESP32 Arduino core. Credit it in both firmwares and in the bible.
//
// Rules that hold for all of it:
//   - The box enforces every cat rule (speed, travel, 20 minutes, rest). The 154 only asks and shows.
//   - No key and no board address ever goes into a journal line or onto a screen.
//
// Written by Julien and Claude the AI
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define EK_LINK_VERSION "1.1.2"

// ======================================================================== radio
#define EK_LINK_CHANNEL      1      // fixed. The box never uses Wi-Fi; the 154 turns its Wi-Fi off and moves here.
#define EK_LINK_PROTO        1      // byte 2 of every packet. Another value: ignored, never answered.

// The primary key of ESP-NOW (PMK) is the same on every board and is public here. It protects
// nothing on its own: the secret is the local key (LMK) the box makes at pairing, one per pair.
static const uint8_t EK_LINK_PMK[16] = { 'E','!','n','k','s','t','e','r','-','l','i','n','k','-','v','1' };

// ======================================================================== timings
#define EK_LINK_HEARTBEAT_MS     1000   // the 154 sends a heartbeat every second while the cat toy mode is open
#define EK_LINK_TIMEOUT_MS       3000   // the box: no packet for this long, the lure hides and the box goes back to its own mode
#define EK_LINK_STOP_REPEAT      3      // stop is sent this many times with one seq; it acts once
#define EK_LINK_IDLE_SLEEP_S     60     // the 154: no press for this long closes the mode and sleeps
#define EK_LINK_HAND_MIN         20     // a budget of 20 minutes that counts only while hand mode runs;
                                        // a session cut short keeps the rest (Julien, 15.10)
#define EK_LINK_REST_AFTER_MIN   25     // once the budget is used up, the box rests 25 minutes; the budget
                                        // refills after them, or after any 25 minutes with no hand mode
#define EK_LINK_PAIR_WINDOW_S    30     // the box's pairing window
#define EK_LINK_BOX_PAIR_HOLD_S  5      // the box: BOOT held 5 s opens the window
#define EK_LINK_BOX_CLEAR_HOLD_S 10     // the box: BOOT held 10 s clears the pairing
// The 154's stop key: BOOT, GPIO 0, while the cat toy mode is open (MARK everywhere else).

// ======================================================================== the packet
// Every packet is 12 bytes, little endian:
//   0-1  magic     "EK"
//   2    proto     EK_LINK_PROTO
//   3    type      EkLinkType
//   4-5  seq       counts up; an answer carries the seq it answers
//   6-7  arg       move: -1 or +1; answer: EkLinkCode; other types: 0
//   8    minutes   in any answer, what is left of the 20-minute budget, whatever the mode;
//                  with code 2 only, the minutes until hand mode opens again. 0 in other packets
//   9    mode      answer only: EkLinkMode; 0 otherwise
//   10-11 reserved zero
// One exception: "hello back" is 28 bytes, the 12 above followed by the box's 16-byte local key.
// The board address is never in a packet: ESP-NOW hands it to the receiver with each packet.
#define EK_LINK_LEN       12
#define EK_LINK_PAIR_LEN  28
#define EK_LINK_KEY_LEN   16

enum EkLinkType : uint8_t {
  EK_LINK_HELLO      = 1,   // 154 to box: pairing (broadcast, unencrypted), then a link check (encrypted)
  EK_LINK_HELLO_BACK = 2,   // box to 154: pairing answer, carries the key
  EK_LINK_MOVE       = 3,   // 154 to box: one step, arg -1 or +1
  EK_LINK_WIGGLE     = 4,   // 154 to box: a short pattern the box knows
  EK_LINK_HIDE       = 5,   // 154 to box: the lure goes back into the box
  EK_LINK_STOP       = 6,   // 154 to box: everything stops at once
  EK_LINK_HEARTBEAT  = 7,   // 154 to box: every second while the mode is open
  EK_LINK_ANSWER     = 8,   // box to 154: the answer to one packet
};

enum EkLinkCode : uint8_t {   // the box's answer (the cat toy chat, 15.10)
  EK_LINK_OK          = 0,
  EK_LINK_NOT_PAIRED  = 1,
  EK_LINK_SESSION_OVER = 2,   // the 20-minute budget is used up; 'minutes' says when hand mode opens again
  EK_LINK_SWAT        = 3,    // hidden by a swat
  EK_LINK_STOPPED     = 4,
};

enum EkLinkMode : uint8_t {   // confirmed by the cat toy chat, 15.10
  EK_LINK_MODE_PLAYING = 0,   // playing on its own
  EK_LINK_MODE_RESTING = 1,
  EK_LINK_MODE_HAND    = 2,
  EK_LINK_MODE_PAIRING = 3,
  EK_LINK_MODE_WAITING = 4,   // its own mode, no cat in sight (asleep until the motion sensor sees one)
};

struct EkLinkPacket {
  uint8_t  type = 0;
  uint16_t seq = 0;
  int16_t  arg = 0;
  uint8_t  minutes = 0;
  uint8_t  mode = 0;
};

static inline size_t ekLinkEncode(const EkLinkPacket &p, uint8_t *out, size_t n) {
  if (n < EK_LINK_LEN) return 0;
  uint16_t a = (uint16_t)p.arg;
  out[0] = 'E'; out[1] = 'K'; out[2] = EK_LINK_PROTO; out[3] = p.type;
  out[4] = (uint8_t)(p.seq & 0xFF); out[5] = (uint8_t)(p.seq >> 8);
  out[6] = (uint8_t)(a & 0xFF);     out[7] = (uint8_t)(a >> 8);
  out[8] = p.type == EK_LINK_ANSWER ? p.minutes : 0;
  out[9] = p.type == EK_LINK_ANSWER ? p.mode : 0;
  out[10] = 0; out[11] = 0;
  return EK_LINK_LEN;
}

// The box's pairing answer: the packet, then the key.
static inline size_t ekLinkEncodeHelloBack(uint16_t seq, const uint8_t key[EK_LINK_KEY_LEN], uint8_t *out, size_t n) {
  if (n < EK_LINK_PAIR_LEN) return 0;
  EkLinkPacket p; p.type = EK_LINK_HELLO_BACK; p.seq = seq;
  ekLinkEncode(p, out, n);
  memcpy(out + EK_LINK_LEN, key, EK_LINK_KEY_LEN);
  return EK_LINK_PAIR_LEN;
}

// false: not ours, another protocol version, a wrong length, an unknown type, or a move other than -1 or +1.
// 'key' receives the key of a hello back when it is not NULL.
static inline bool ekLinkDecode(const uint8_t *in, size_t n, EkLinkPacket &p, uint8_t *key = NULL) {
  if (n < EK_LINK_LEN || in[0] != 'E' || in[1] != 'K' || in[2] != EK_LINK_PROTO) return false;
  uint8_t t = in[3];
  if (t < EK_LINK_HELLO || t > EK_LINK_ANSWER) return false;
  if (t == EK_LINK_HELLO_BACK ? n != EK_LINK_PAIR_LEN : n != EK_LINK_LEN) return false;
  p = EkLinkPacket();
  p.type = t;
  p.seq = (uint16_t)(in[4] | (in[5] << 8));
  p.arg = (int16_t)(uint16_t)(in[6] | (in[7] << 8));
  if (t == EK_LINK_ANSWER) { p.minutes = in[8]; p.mode = in[9]; }
  if (t == EK_LINK_MOVE && p.arg != 1 && p.arg != -1) return false;
  if (t == EK_LINK_HELLO_BACK && key) memcpy(key, in + EK_LINK_LEN, EK_LINK_KEY_LEN);
  return true;
}

// ======================================================================== sequence filter
// Acts on each seq once: a repeated stop, or a packet the radio delivered twice, is dropped.
// After a silence longer than the timeout, any seq is taken again, so a 154 that restarted
// (and counts from 0 again) is heard at once. A hello also resets it.
struct EkLinkSeq {
  bool have = false;
  uint16_t last = 0;
  uint32_t atMs = 0;
  bool accept(uint16_t seq, uint32_t nowMs) {
    bool fresh = !have || (uint32_t)(nowMs - atMs) > EK_LINK_TIMEOUT_MS || (int16_t)(seq - last) > 0;
    if (fresh) { have = true; last = seq; atMs = nowMs; }
    else if (seq == last) atMs = nowMs;     // a repeat still shows the 154 is there
    return fresh;
  }
  void reset() { have = false; }
};

// ======================================================================== heartbeat watch (the box)
// heard() on every packet from the paired 154; lost() true means: hide the lure, back to the box's own mode.
struct EkLinkWatch {
  bool live = false;
  uint32_t atMs = 0;
  void heard(uint32_t nowMs) { live = true; atMs = nowMs; }
  bool lost(uint32_t nowMs) {
    if (live && (uint32_t)(nowMs - atMs) > EK_LINK_TIMEOUT_MS) { live = false; return true; }
    return false;
  }
};

// ======================================================================== words for the 154's screen
// Short enough for the 200 px screen. French for the school build; other languages come with the language release.
static inline const char *ekLinkCodeText(uint8_t c, bool fr = false) {
  switch (c) {
    case EK_LINK_OK:           return "ok";
    case EK_LINK_NOT_PAIRED:   return fr ? "non appair\xC3\xA9" : "not paired";
    case EK_LINK_SESSION_OVER: return fr ? "s\xC3\xA9" "ance finie" : "session over";
    case EK_LINK_SWAT:         return fr ? "cach\xC3\xA9\xC2\xA0" ": coup de patte" : "hidden: a swat";
    case EK_LINK_STOPPED:      return fr ? "arr\xC3\xAAt\xC3\xA9" : "stopped";
    default:                   return "?";
  }
}
static inline const char *ekLinkModeText(uint8_t m, bool fr = false) {
  switch (m) {
    case EK_LINK_MODE_PLAYING: return fr ? "joue" : "playing";
    case EK_LINK_MODE_RESTING: return fr ? "repos" : "resting";
    case EK_LINK_MODE_HAND:    return fr ? "main" : "hand";
    case EK_LINK_MODE_PAIRING: return fr ? "appairage" : "pairing";
    case EK_LINK_MODE_WAITING: return fr ? "attente" : "waiting";
    default:                   return "?";
  }
}

// When the box does not answer. A sleeping box cannot hear the 154; a wave wakes it through its
// motion sensor (Julien, 15.10, choice a). Two lines on the 200 px screen.
static inline const char *ekLinkNoAnswerText(bool fr = false) {
  return fr ? "pas de r\xC3\xA9ponse\xC2\xA0" ": la bo\xC3\xAEte dort, agitez la main devant elle"
            : "no answer: the box is asleep, wave in front of it";
}

// ======================================================================== settings names (the 154)
// In the shared namespace "einkster", 15 characters at most. Personal: never in the journal.
#define EK_SET_CT_PEER "ctPeer"   // the box's address, 6 bytes
#define EK_SET_CT_KEY  "ctKey"    // the local key, 16 bytes
