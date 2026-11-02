"""Assemble the E!nkster 397 sketch: our files + the shared files + the packs + the power chip library + secrets.
Usage (from the repository root): python3 397/prepare397.py <waveshare_3.97_repo_dir> <out_sketch_dir>
  - einkster397.ino comes from 397/; ek_shared.h and fonts.h from the root, as the 154 uses them.
  - packs.h is cut out of einkster154.ino (from the "built-in packs" line to the end), so both boards carry the same text.
  - XPowersLib (Lewis He, MIT) is the copy inside Waveshare's own 3.97 demo, taken unchanged.
Keys come from config.h at the repository root (listed in .gitignore; copy config.example.h) or from the environment.
EK_NO_SECRETS=1 builds it with no network and no card inside: everything is set up on the device."""
import json
import os
import shutil
import sys

ws, out = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
os.makedirs(out, exist_ok=True)

shutil.copy(os.path.join(here, "einkster397.ino"), out)
shutil.copy(os.path.join(here, "partitions.csv"), out)   # 16 MB: two 4 MB slots, the rest for files
for f in ("ek_shared.h", "fonts.h"):
    shutil.copy(os.path.join(root, f), out)

# ---- the packs, from the 154's sketch
src154 = open(os.path.join(root, "einkster154.ino"), encoding="utf-8").read()
mark = "// ---------------------------------------------------------------- built-in packs"
if src154.count(mark) != 1:
    sys.exit("packs: the built-in packs line was not found once in einkster154.ino")
packs = src154[src154.index(mark):]
for name in ("AT", "VU", "YO", "CA", "GU", "WF", "WFD", "WFF", "EZ"):
    for part in ("EDITION", "CARDS"):
        if f"const char {name}_{part}[] =" not in packs:
            sys.exit(f"packs: {name}_{part} missing in einkster154.ino")
with open(os.path.join(out, "packs.h"), "w", encoding="utf-8") as f:
    f.write("// cut from einkster154.ino by prepare397.py: the same packs on both boards\n#pragma once\n")
    f.write(packs)

# ---- the power chip library, unchanged
lib = os.path.join(ws, "ESP-IDF", "08_ESP32-S3_e-Paper-3.97", "components", "axpPower", "src")
if not os.path.isfile(os.path.join(lib, "XPowersLib.h")):
    sys.exit("XPowersLib not found in the Waveshare repository at " + lib)
shutil.copytree(lib, os.path.join(out, "src", "XPowersLib"), dirs_exist_ok=True)

# ---- secrets
env = os.environ
config = os.path.join(root, "config.h")
if os.path.isfile(config) and env.get("EK_NO_SECRETS") != "1" and not env.get("EK_WIFI_SSID"):   # keys in the environment win (the bench)
    shutil.copy(config, os.path.join(out, "secrets.h"))       # your own keys, from config.h
    print("397 sketch assembled in", out, "(keys from config.h)")
    sys.exit(0)
nokeys = env.get("EK_NO_SECRETS") == "1"            # a build with no network and no card inside: set up on the device
missing = [k for k in ("EK_WIFI_SSID", "EK_WIFI_PASS") if not env.get(k)]
if missing and not nokeys:
    sys.exit("no config.h and missing keys: " + ", ".join(missing) + " (or build with EK_NO_SECRETS=1)")
if nokeys:
    env = {"EK_WIFI_SSID": "", "EK_WIFI_PASS": "", "EK_CARD": ""}
with open(os.path.join(out, "secrets.h"), "w") as f:  # JSON escaping is valid C escaping here
    f.write(f"#define EK_WIFI_SSID {json.dumps(env['EK_WIFI_SSID'])}\n")
    f.write(f"#define EK_WIFI_PASS {json.dumps(env['EK_WIFI_PASS'])}\n")
    f.write(f"#define EK_CARD {json.dumps(env.get('EK_CARD') or '')}\n")
print("397 sketch assembled in", out)
