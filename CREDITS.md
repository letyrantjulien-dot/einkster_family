# Credits

E!nkster stands on the shoulders of giants. This file names every library, service, dataset and typeface the code in this repository uses, with its licence. Third-party code keeps its own licence and header.

## Libraries and code
| What | Used for | By | Licence |
|---|---|---|---|
| ESP32 Arduino core 3.2.0 and ESP-IDF 5.4 | the whole firmware: Wi-Fi, TLS, flash file system, sleep, Bluetooth, ESP-NOW, SD card, web server for the set-up page | Espressif Systems | Apache-2.0 / LGPL-2.1 (core parts), see [arduino-esp32](https://github.com/espressif/arduino-esp32) |
| BLE library of the ESP32 Arduino core, on Bluedroid | the 154's Cl!cker keyboard (`ek_hid.h`) and the 397's keyboard | Espressif, from Neil Kolban's ESP32 BLE library | Apache-2.0 |
| ESP-NOW | the radio link of `ek_link.h` (154 and the peek-a-boo box) | Espressif | Apache-2.0 |
| espressif/qrcode component | QR codes on the paper (Catalog, set-up page) | Espressif, built on Project Nayuki's QR Code generator | Apache-2.0 / MIT |
| Helix MP3 decoder | FIP radio over Icecast (154) | RealNetworks, bundled with the ESP32 core | RPSL |
| Helix AAC decoder | FIP radio over HLS (154), fetched by `prepare.py` from [pschatzmann/arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix) at a pinned commit | RealNetworks; Arduino packaging by Phil Schatzmann | RPSL |
| Waveshare ESP32-S3-Touch-ePaper-1.54 drivers (display, power, I2C, codec board) | the 154's paper, power, touch bus and audio; fetched at build time from [waveshareteam/ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54), never copied here and never changed | Waveshare | as stated in Waveshare's repository |
| esp_codec_dev (inside Waveshare's example) | the 154's ES8311 audio codec | Espressif | Apache-2.0 |
| XPowersLib | the 397's AXP2101 power chip; the copy Waveshare ships in [waveshareteam/ESP32-S3-ePaper-3.97](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97), fetched at build time | Lewis He | MIT |
| python-libzim and libzim | reading Kiwix archives in `397/shelf/shelf_convert.py` | openZIM / Kiwix | GPL-3.0-or-later |
| WString, stdlib_noniso (in `397/tests/host/core/`) | the 397's bench on a computer | Hernando Barragan, Paul Stoffregen, Ivan Grokhotkov and the Arduino / ESP8266 / ESP32 core authors | LGPL-2.1-or-later, headers kept |
| QR Code generator, C (in `397/tests/host/ext/`) | QR codes in the 397's bench | Project Nayuki | MIT, headers kept |

## Typeface
- **DejaVu Sans Bold**, drawn into bitmap glyphs in `fonts.h`. DejaVu fonts, from Bitstream Vera (Bitstream Vera licence, free to embed).

## Services the boards reach
| Service | Used for | Terms |
|---|---|---|
| [Open-Meteo](https://open-meteo.com) (forecast, marine and air-quality APIs) | weather on both boards | data CC BY 4.0, no key, no account |
| [Radio France](https://www.radiofrance.fr), FIP | the 154's radio tile and FIP alarm (public HLS and Icecast streams) | the station's own streams, played as they are |
| [PTB](https://www.ptb.de) time servers and the [NTP Pool](https://www.ntppool.org) | the clock | public time service |
| Your own relay | the 154's Ask, Der Die Das, Oracle and Jar tiles and the spoken weather question | brought by you, see `config.example.h`; the InvisibleChaos relay is private |
| Kaffeesatz (an InvisibleChaos app) | the 154's daily edito tile, optional | needs your own channel code |

No Google service, font or server is used anywhere.

## Content of the built-in packs
The packs (Atlantid!a, Vulcan!a, Yog!a, C@tlas, Guineapedi@, W@terfacts!, Edit!on Zero) name their sources on each card and on their colophon card. The main ones: Wikipedia (CC BY-SA 4.0), the U.S. Geological Survey, the Smithsonian Global Volcanism Program, NASA FIRMS, Copernicus Marine, the U.S. National Center for Complementary and Integrative Health, the MSD Veterinary Manual, the ASPCA, International Cat Care and Encyclopaedia Britannica.

## Archives on the shelf
Kiwix and openZIM make the archives the 397 reads from its card. Their content keeps its own licence (Wikipedia's text: CC BY-SA 4.0), which the converter writes into each archive's `about.txt` and the 397 shows with every article.

## Hardware
- **Waveshare** ESP32-S3-Touch-ePaper-1.54 (V2) and ESP32-S3-ePaper-3.97, with their open example code.

Written by Julien and Claude the AI
