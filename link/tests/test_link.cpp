// Bench for ek_link.h. Build and run: sh run.sh
// Written by Julien and Claude the AI
#include "../../ek_link.h"
#include <stdio.h>
#include <initializer_list>

static int checks = 0, failed = 0;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
  uint8_t b[64], key[16], got[16];
  EkLinkPacket p, q;

  // sizes
  CHECK(EK_LINK_LEN == 12 && EK_LINK_PAIR_LEN == 28 && sizeof EK_LINK_PMK == 16);

  // every command round trips
  const uint8_t types[] = { EK_LINK_HELLO, EK_LINK_WIGGLE, EK_LINK_HIDE, EK_LINK_STOP, EK_LINK_HEARTBEAT };
  for (uint8_t t : types) {
    p = EkLinkPacket(); p.type = t; p.seq = 513;
    CHECK(ekLinkEncode(p, b, sizeof b) == 12);
    CHECK(ekLinkDecode(b, 12, q) && q.type == t && q.seq == 513 && q.arg == 0);
  }

  // move: -1 and +1 pass, anything else is refused
  for (int a : { -1, 1 }) {
    p = EkLinkPacket(); p.type = EK_LINK_MOVE; p.seq = 7; p.arg = (int16_t)a;
    ekLinkEncode(p, b, sizeof b);
    CHECK(ekLinkDecode(b, 12, q) && q.arg == a);
  }
  for (int a : { 0, 2, -2, 300 }) {
    p = EkLinkPacket(); p.type = EK_LINK_MOVE; p.arg = (int16_t)a;
    ekLinkEncode(p, b, sizeof b);
    CHECK(!ekLinkDecode(b, 12, q));
  }

  // the answer carries code, minutes and mode
  p = EkLinkPacket(); p.type = EK_LINK_ANSWER; p.seq = 65535; p.arg = EK_LINK_SESSION_OVER; p.minutes = 25; p.mode = EK_LINK_MODE_RESTING;
  ekLinkEncode(p, b, sizeof b);
  CHECK(ekLinkDecode(b, 12, q) && q.seq == 65535 && q.arg == EK_LINK_SESSION_OVER && q.minutes == 25 && q.mode == EK_LINK_MODE_RESTING);
  CHECK(b[10] == 0 && b[11] == 0);

  // minutes and mode are zero on anything but an answer
  p = EkLinkPacket(); p.type = EK_LINK_HEARTBEAT; p.minutes = 9; p.mode = 2;
  ekLinkEncode(p, b, sizeof b);
  CHECK(b[8] == 0 && b[9] == 0);

  // little endian on the air
  p = EkLinkPacket(); p.type = EK_LINK_HEARTBEAT; p.seq = 0x1234;
  ekLinkEncode(p, b, sizeof b);
  CHECK(b[0] == 'E' && b[1] == 'K' && b[2] == 1 && b[4] == 0x34 && b[5] == 0x12);

  // hello back carries the key, and only at 28 bytes
  for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 17 + 3);
  CHECK(ekLinkEncodeHelloBack(42, key, b, sizeof b) == 28);
  memset(got, 0, sizeof got);
  CHECK(ekLinkDecode(b, 28, q, got) && q.type == EK_LINK_HELLO_BACK && q.seq == 42 && !memcmp(got, key, 16));
  CHECK(!ekLinkDecode(b, 12, q));                          // cut short
  CHECK(ekLinkEncodeHelloBack(1, key, b, 20) == 0);         // no room

  // refused: not ours, other version, wrong length, unknown type
  p = EkLinkPacket(); p.type = EK_LINK_STOP; ekLinkEncode(p, b, sizeof b);
  uint8_t c[12]; memcpy(c, b, 12);
  c[0] = 'X';                    CHECK(!ekLinkDecode(c, 12, q)); memcpy(c, b, 12);
  c[2] = 2;                      CHECK(!ekLinkDecode(c, 12, q)); memcpy(c, b, 12);
  c[3] = 0;                      CHECK(!ekLinkDecode(c, 12, q)); memcpy(c, b, 12);
  c[3] = 9;                      CHECK(!ekLinkDecode(c, 12, q));
  CHECK(!ekLinkDecode(b, 11, q));
  CHECK(!ekLinkDecode(b, 13, q));
  CHECK(ekLinkEncode(p, b, 11) == 0);

  // sequence: a stop sent three times acts once
  EkLinkSeq s;
  CHECK(s.accept(10, 1000));
  CHECK(!s.accept(10, 1010));
  CHECK(!s.accept(10, 1020));
  CHECK(s.accept(11, 1100));
  CHECK(!s.accept(9, 1200));                                // older
  // across the wrap
  s.reset(); CHECK(s.accept(65534, 0)); CHECK(s.accept(65535, 10)); CHECK(s.accept(0, 20)); CHECK(s.accept(1, 30));
  // a 154 that restarted counts from 0 again: taken after a silence longer than the timeout
  s.reset(); CHECK(s.accept(500, 0)); CHECK(!s.accept(0, 2000)); CHECK(s.accept(0, 2000 + EK_LINK_TIMEOUT_MS + 1));
  // repeats keep the link fresh, so the silence is counted from the last repeat
  s.reset(); CHECK(s.accept(5, 0)); CHECK(!s.accept(5, 2900)); CHECK(!s.accept(4, 5800));

  // heartbeat watch: lost once after 3 s of silence, not before, not twice
  EkLinkWatch w;
  CHECK(!w.lost(100000));                                   // never heard: nothing to lose
  w.heard(0);
  CHECK(!w.lost(EK_LINK_TIMEOUT_MS));
  CHECK(w.lost(EK_LINK_TIMEOUT_MS + 1));
  CHECK(!w.lost(EK_LINK_TIMEOUT_MS + 2000));
  w.heard(4294967000u);                                     // across the millis() wrap
  CHECK(!w.lost(1000));
  CHECK(w.lost(4000));

  // words: every code and mode has one, in both languages, short enough for the screen
  for (int i = 0; i <= 4; i++) {
    CHECK(strcmp(ekLinkCodeText(i), "?") && strcmp(ekLinkCodeText(i, true), "?"));
    CHECK(strlen(ekLinkCodeText(i)) <= 20 && strlen(ekLinkCodeText(i, true)) <= 26);
  }
  for (int i = 0; i <= 4; i++) CHECK(strcmp(ekLinkModeText(i), "?") && strcmp(ekLinkModeText(i, true), "?"));
  CHECK(EK_LINK_MODE_WAITING == 4 && !strcmp(ekLinkModeText(4), "waiting") && !strcmp(ekLinkModeText(4, true), "attente"));
  CHECK(!strcmp(ekLinkModeText(5), "?"));
  // the answer carries mode 4
  p = EkLinkPacket(); p.type = EK_LINK_ANSWER; p.arg = EK_LINK_OK; p.mode = EK_LINK_MODE_WAITING;
  ekLinkEncode(p, b, sizeof b);
  CHECK(ekLinkDecode(b, 12, q) && q.mode == EK_LINK_MODE_WAITING);
  // the no-answer sentence, in both languages, fits two lines of the screen
  CHECK(!strcmp(ekLinkNoAnswerText(), "no answer: the box is asleep, wave in front of it"));
  CHECK(strlen(ekLinkNoAnswerText()) <= 56 && strlen(ekLinkNoAnswerText(true)) <= 72);
  CHECK(strstr(ekLinkNoAnswerText(true), "\xC2\xA0:") != NULL);   // French space before the colon
  CHECK(!strcmp(ekLinkCodeText(99), "?"));

  // the rulings are written in
  CHECK(EK_LINK_CHANNEL == 1 && EK_LINK_HAND_MIN == 20 && EK_LINK_REST_AFTER_MIN == 25);
  CHECK(EK_LINK_HEARTBEAT_MS == 1000 && EK_LINK_TIMEOUT_MS == 3000 && EK_LINK_STOP_REPEAT == 3);

  // settings names fit the 15-character limit
  CHECK(strlen(EK_SET_CT_PEER) <= 15 && strlen(EK_SET_CT_KEY) <= 15);

  printf("ek_link.h %s: %d checks, %d failed\n", EK_LINK_VERSION, checks, failed);
  return failed ? 1 : 0;
}
