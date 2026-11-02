#!/bin/bash
# The 397 bench: assembles the sketch the way the build does (bench secrets), then compiles it for this computer.
#   ./build.sh && ./bench397 out && python3 sheet.py out sheet.png
# Needs: g++, python3, the Waveshare 3.97 repository (WS397, for the power chip library the board build uses),
# and the 154's einkster154.ino, ek_shared.h, fonts.h next to the 397 folder (the repository's layout).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); REPO=${REPO:-$(cd "$HERE/../../.." && pwd)}; WS397=${WS397:-$REPO/ws397}   # the clone the README makes
B=$HERE/build; rm -rf $B; mkdir -p $B
EK_WIFI_SSID='Bench|Bench2' EK_WIFI_PASS='bench-pass|bench-pass2' EK_CARD='Bench Name|bench@example.org|+000000000000' \
  python3 $REPO/397/prepare397.py $WS397 $B/sketch > /dev/null
gcc -O1 -w -c $HERE/ext/qrcodegen.c -o $B/qr.o
g++ -std=gnu++17 -O1 -w -I$HERE/core -c $HERE/core/WString.cpp -o $B/wstring.o
gcc -O1 -w -I$HERE/core -c $HERE/core/stdlib_noniso.c -o $B/noniso.o
g++ -std=gnu++17 -O1 -Wall -Wno-unused-function -Wno-unused-variable -fpermissive -I$HERE -I$HERE/core -I$HERE/ext -I$B/sketch \
  -o $HERE/bench397 $HERE/host.cpp $B/qr.o $B/wstring.o $B/noniso.o 2>&1 | grep -E "error|warning" | head -40
[ -x $HERE/bench397 ] && echo "built bench397"
