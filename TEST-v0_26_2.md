# E!nkster 154 v0.26.2: GP2 released when the sensor is off
29 October 2026. Julien's ship of 29.10: "Shall we try this high impedance thing on the GP2? You can ship." One change, both editions. `ek_shared.h` 1.0.3 unchanged.

## The question this build answers
Six runs with the sensor module's power wire on GP2 (runs 20 to 25, 26 to 28.10) ended in a power cut during deep sleep. None on 3V3. The firmware's pin states in sleep are the same in both wirings, so the code running cannot be what differs; what differs is what GP2 is connected to, and what the firmware leaves on that pin: up to v0.26.1, **driven low and held low** through sleep, which pulled the module's supply to ground while its bus lines still saw 3.3 V. v0.26.2 changes that one thing.

## What changed
- **Off now means released.** When the sensor is not being read, GP2 is an input with no pull (high impedance), and it is held that way through sleep. The pin sources and sinks nothing. For a reading, as before: driven high, 50 ms, read, released.
- **At wake,** the pin is set to input before the sleep hold lets go, so it never passes through "output low".
- **The night light** (GP2 held high through sleep) is unchanged.
- **The log** says at every start: "GP2 is released when the sensor is off (v0.26.2), not driven low". The Sensors page says "power switched on GP2, released when off".
- The breadcrumbs of v0.25.6 and v0.26.1 stay as they are.

## What to expect, and what it costs
- With GP2 released, the module is no longer reverse-fed against a grounded supply pin. Its rail floats to wherever the bus diodes leave it (a volt or two, not checked), so **its LED may glow faintly** between readings, and the night drain will sit somewhere between the GP2-low figure (1 to 6 mV/h) and the 3V3 figure (10 to 18 mV/h). The 30-minute log will say.
- If the sensor stops answering at readings (a half-fed chip can refuse to start), the log says "no pressure sensor" at the sample and the Sensors page shows it. Say so.

## The test
1. Put the purple wire back on **GP2**.
2. Flash `einkster154-v0_26_2-classic-update.bin` (from Actions, after the push you ask for) at **0x10000**. Until then, the InvisibleOcean build here is the same code with the French default and no keys.
3. Switch on, check the Sensors page: "power switched on GP2, released when off".
4. **Two days on battery, as before.** Then send the log.
   - **No cut in two days:** the active low was the trigger; v0.26.2 stays, GP2 wiring stays, the drain question is answered by the 30-minute log.
   - **A cut:** it is the wire, the socket or the module, not the pin state; back to 3V3, and the module's LED comes off or the module changes.

## What was checked
- **Builds:** Classic 2,376,090 bytes, InvisibleOcean 1,782,438 bytes, 8 MB layout, no warning in our file (checked).
- **The bench:** 926 checks on the cut-out parts, 0 failed. **The simulator:** Classic 146 checks, InvisibleOcean 80 checks, 0 failed, with GP2 modelled so that a released pin counts as "not high": the sensor is found on GP2, GP2 is high only for the readings (under 0.2 s each over 6 hours), the night light holds it high, the cut breadcrumbs still work (checked).
- **Not checked:** the real pin on the board; what the module's rail does when released; the LED; the drain.

## Files
- `einkster154.ino` v0.26.2 for the repository (`ek_shared.h` unchanged).
- `einkster154-v0_26_2-invisibleocean-update.bin` / `-full.bin`, built here.
- The Classic binaries with your keys come from Actions after a push, on your word.

Written by Julien and Claude the AI
