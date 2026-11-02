# ek_link.h 1.1.2: the link between the E!nkster 154 and the peek-a-boo box
15 October 2026. From the firmware chat, for the cat toy chat and for the 154's own firmware.

## What is in it
- **The radio:** ESP-NOW on fixed channel 1. The box never uses Wi-Fi. The 154 turns its Wi-Fi off while the cat toy mode is open.
- **The packet:** 12 bytes. Magic "EK", protocol 1, type, seq, arg, minutes, mode, two reserved bytes. A packet with another magic or protocol is ignored, never answered.
- **The codes:** 0 ok, 1 not paired, 2 hand session over, 3 hidden by a swat, 4 stopped.
- **The modes:** 0 playing, 1 resting, 2 hand, 3 pairing, 4 waiting. Confirmed by the cat toy chat on 15.10. "Waiting" is the box in its own mode with no cat in sight.
- **The timings:** heartbeat every second, timeout 3 s, stop sent 3 times, 60 s idle sleep on the 154, a budget of 20 minutes that counts only while hand mode runs, and a session cut short keeps the rest; once the budget is used up, the box rests 25 minutes, and the budget refills after them, or after any 25 minutes with no hand mode (Julien, 15.10). Pairing window 30 s; BOOT held 5 s opens it, 10 s clears.
- **Three helpers:**
  - `ekLinkEncode` / `ekLinkDecode` for the packet.
  - `EkLinkSeq` acts on each seq once, so a repeated stop counts once. After a silence longer than 3 s it takes any seq, so a restarted 154 is heard at once.
  - `EkLinkWatch` tells the box when the heartbeat is lost.
- **Words for the 154's screen:** in English and French, including the "no answer" sentence: "no answer: the box is asleep, wave in front of it". A sleeping box cannot hear the 154; a wave wakes it through its motion sensor (Julien, 15.10, choice a). BOOT inside the box is for pairing only.
- **Two settings names on the 154:** the box's address and the key. Neither ever goes into the field log.

## Changes
- **1.1.2 (15.10):** words only. The minutes byte carries the budget left in every answer, whatever the mode. Nothing in the code changed.
- **1.1.1 (15.10):** words only. The hand budget ruling, in the cat toy chat's words. Nothing in the code changed.
- **1.1.0 (15.10):** mode 4 "waiting"; the "no answer" sentence. Nothing else changed: the packet is the same, so 1.0.0 and 1.1.0 understand each other.

## Two things that differ from reply v3
1. **"Hello back" is 28 bytes, not 12.** It carries the box's 16-byte key after the packet. A 12-byte packet has no room for the key. Every other packet stays at 12 bytes.
2. **The minutes byte.** In any answer, whatever the mode, it is what is left of the 20-minute budget. With code 2 only, it is the minutes until hand mode opens again. So the 154 can show the budget left as soon as the box answers its first hello. With code 2 ("session over") it is the minutes until hand mode opens again (ruling b).

## Pairing, step by step
1. The box sits on channel 1 with its window open (5 s on BOOT, or by itself when unpaired).
2. The 154 sends hello, broadcast and unencrypted.
3. The box makes a random 16-byte key, adds the 154 as an encrypted peer, and answers hello back with the key.
4. The 154 stores the box's address and the key and adds the box as an encrypted peer. From then on, every packet is encrypted.
5. The key crosses the air once, during the window. Julien accepted this on 15.10.

The primary key (PMK) in the file is public and the same everywhere. The secret is the per-pair key.

## Checked
- The bench: 78 checks, none failed. Built with g++ 13 and strict warnings (`-Wall -Wextra -Werror`).
- It builds next to `ek_shared.h` with no name clash.

## Not checked
- Building with the ESP32 compiler. It uses no library, so I expect no trouble.
- Anything on a device: ESP-NOW range in the flat, how fast a stop arrives, the current drawn.

## Credits
ESP-NOW, by Espressif, in the ESP32 Arduino core. Credit it in both firmwares, in the bible and in the cat toy app.

Written by Julien and Claude the AI
