"""Assemble the Einkster 154 sketch: our files + Waveshare's V2 drivers + secrets.
Usage: python3 prepare.py <waveshare_repo_dir> <out_sketch_dir>
Keys and private settings come from config.h at the repository root (listed in .gitignore, never committed;
copy config.example.h to start) or from the environment. Without either, build the edition with no keys:
EK_EDITION=invisibleocean python3 prepare.py <waveshare_repo_dir> <out_sketch_dir>"""
import json
import os
import shutil
import sys

ws, out = sys.argv[1], sys.argv[2]
src = os.path.join(ws, "02_Example", "Arduino", "08_Audio_Test")
os.makedirs(os.path.join(out, "src"), exist_ok=True)

for d in ("display", "power", "i2c_bsp", "esp_codec_dev", "codec_board"):
    shutil.copytree(os.path.join(src, "src", d), os.path.join(out, "src", d), dirs_exist_ok=True)
shutil.copy(os.path.join(src, "user_config.h"), out)
for f in ("einkster154.ino", "audio.c", "audio.h", "fonts.h", "ek_shared.h", "ek_hid.h"):   # ek_hid.h: v0.25.2, the Cl!cker
    shutil.copy(f, out)


# ---- Helix AAC decoder (RealNetworks RPSL, same family as the core's Helix MP3 decoder), for FIP over HLS.
# Only the Helix sources are taken, pinned to one commit; three tiny shims replace the wrapper's headers.
HELIX_REPO = "pschatzmann/arduino-libhelix"
HELIX_SHA = "5c0a04302dbd661a56408eeb1756e01dd8d33ba1"
def fetch_helix_aac(dest):
    import io, urllib.request, zipfile
    url = f"https://codeload.github.com/{HELIX_REPO}/zip/{HELIX_SHA}"
    data = urllib.request.urlopen(url, timeout=60).read()
    z = zipfile.ZipFile(io.BytesIO(data))
    os.makedirs(os.path.join(dest, "utils"), exist_ok=True)
    n = 0
    for name in z.namelist():
        if "/src/libhelix-aac/" in name and not name.endswith("/"):
            base = name.split("/src/libhelix-aac/")[1]
            if "/" in base or not base.endswith((".c", ".h", ".txt")):
                continue
            with open(os.path.join(dest, base), "wb") as f:
                f.write(z.read(name))
            n += 1
    if n < 20:
        sys.exit("helix-aac: sources not found in the pinned archive")
    open(os.path.join(dest, "ConfigHelix.h"), "w").write(
        "#pragma once\n#define HELIX_FEATURE_AUDIO_CODEC_AAC_SBR\n")
    open(os.path.join(dest, "utils", "helix_pgm.h"), "w").write(
        "#pragma once\n#define PROGMEM\n#define pgm_read_byte(a) (*(const unsigned char *)(a))\n"
        "#define pgm_read_word(a) (*(const unsigned short *)(a))\n")
    open(os.path.join(dest, "utils", "helix_memory.h"), "w").write(
        "#pragma once\n#ifdef __cplusplus\nextern \"C\" {\n#endif\nvoid *helix_malloc(int size);\n"
        "void helix_free(void *ptr);\n#ifdef __cplusplus\n}\n#endif\n")
    os.makedirs(os.path.join(dest, "hlxclib"), exist_ok=True)
    for h in ("stdlib", "string"):
        open(os.path.join(dest, "hlxclib", h + ".h"), "w").write(f"#pragma once\n#include <{h}.h>\n")
    open(os.path.join(dest, "helix_shim.c"), "w").write(
        "#include <stdlib.h>\nvoid *helix_malloc(int size) { return malloc(size); }\n"
        "void helix_free(void *ptr) { free(ptr); }\n")
    print(f"helix-aac: {n} files from {HELIX_REPO}@{HELIX_SHA[:7]}")

fetch_helix_aac(os.path.join(out, "src", "helix-aac"))


def patch(path, old, new):
    s = open(path).read()
    if old not in s:
        sys.exit(f"patch anchor not found in {path}: {old[:40]}")
    open(path, "w").write(s.replace(old, new, 1))


# touch chip (FT6336) shares the I2C bus with the RTC and the sensor
cfg = os.path.join(out, "user_config.h")
patch(cfg, "#define I2C_SHTC3_DEV_Address      0x70",
      "#define I2C_SHTC3_DEV_Address      0x70\n#define I2C_TOUCH_DEV_Address      0x38\n"
      "#define TOUCH_RST_PIN GPIO_NUM_7\n#define TOUCH_INT_PIN GPIO_NUM_21")
i2c_c = os.path.join(out, "src", "i2c_bsp", "i2c_bsp.c")
patch(i2c_c, "i2c_master_dev_handle_t shtc3_handle = NULL;",
      "i2c_master_dev_handle_t shtc3_handle = NULL;\ni2c_master_dev_handle_t touch_handle = NULL;")
patch(i2c_c, "ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_handle, &dev_cfg, &shtc3_handle));",
      "ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_handle, &dev_cfg, &shtc3_handle));\n"
      "  dev_cfg.device_address = I2C_TOUCH_DEV_Address;\n"
      "  ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_handle, &dev_cfg, &touch_handle));")
patch(os.path.join(out, "src", "i2c_bsp", "i2c_bsp.h"),
      "extern i2c_master_dev_handle_t shtc3_handle;",
      "extern i2c_master_dev_handle_t shtc3_handle;\nextern i2c_master_dev_handle_t touch_handle;")


env = os.environ
config = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.h")
edition = (env.get("EK_EDITION") or "classic").strip().lower()   # classic | invisibleocean
if edition not in ("classic", "invisibleocean"):
    sys.exit("EK_EDITION must be classic or invisibleocean, not " + edition)
if edition == "classic" and os.path.isfile(config) and not env.get("EK_WIFI_SSID"):   # keys in the environment win
    shutil.copy(config, os.path.join(out, "secrets.h"))       # your own keys, from config.h
    print("edition: classic, keys from config.h")
    print("sketch assembled in", out)
    sys.exit(0)
if edition == "classic":
    missing = [k for k in ("EK_WIFI_SSID", "EK_WIFI_PASS", "EK_RELAY_KEY") if not env.get(k)]
    if missing:
        sys.exit("no config.h and missing keys: " + ", ".join(missing) + " (or build with EK_EDITION=invisibleocean)")
with open(os.path.join(out, "secrets.h"), "w") as f:  # JSON escaping is valid C escaping here
    if edition == "invisibleocean":
        # no secrets in this edition: Wi-Fi is set up on the device, weather comes from Open-Meteo
        f.write("#define EK_INVISIBLEOCEAN 1\n")
        f.write('#define EK_WIFI_SSID ""\n#define EK_WIFI_PASS ""\n#define EK_RELAY_KEY ""\n')
        f.write('#define EK_KS_CHANNEL ""\n#define EK_BRAIN_URL ""\n#define EK_CARD ""\n')
    else:
        f.write(f"#define EK_WIFI_SSID {json.dumps(env['EK_WIFI_SSID'])}\n")
        f.write(f"#define EK_WIFI_PASS {json.dumps(env['EK_WIFI_PASS'])}\n")
        f.write(f"#define EK_RELAY_KEY {json.dumps(env['EK_RELAY_KEY'])}\n")
        card = env.get("EK_CARD") or ""                     # optional: business card, "name|email|phone"
        f.write(f"#define EK_CARD {json.dumps(card)}\n")
        ks = env.get("EK_KS_CHANNEL") or ""                # optional: the Kaffeesatz channel code
        f.write(f"#define EK_KS_CHANNEL {json.dumps(ks)}\n")
        brain = env.get("EK_BRAIN_URL") or ""             # your relay's address: never a default
        f.write(f"#define EK_BRAIN_URL {json.dumps(brain)}\n")
print("edition:", edition)
print("sketch assembled in", out)
