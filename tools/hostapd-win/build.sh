#!/bin/sh
# Build drc-hostap (CONFIG_TENDONIN) as a native Windows hostapd.exe with MSYS2 MinGW-w64.
# Source: the pinned drc-hostap checkout in .local/refs/drc-hostap (see docs/REFERENCES.md).
# Output: .local/build/hostap/hostapd/hostapd.exe. Nothing outside the repo's .local changes.
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -l tools/hostapd-win/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src="$repo/.local/refs/drc-hostap"
out="$repo/.local/build/hostap"
pin=418e5e206786de2482864a0ec3a59742a33b6623

[ "$(git -C "$src" rev-parse HEAD)" = "$pin" ] || { echo "drc-hostap is not at $pin" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out"
git -C "$src" archive "$pin" | tar -x -C "$out"
# The patch is LF; normalise only the files it touches.
for f in hostapd/ctrl_iface.h src/ap/beacon.c src/ap/ieee802_11.c src/ap/wpa_auth_ie.c src/drivers/driver.h src/drivers/drivers.c src/drivers/drivers.mak; do
    sed -i 's/\r$//' "$out/$f"
done
(cd "$out" && git apply -p1 "$here/windows.patch")
cp "$here/hostapd.config" "$out/hostapd/.config"
cp "$here/driver_ziiu.c" "$here/ziiu_net.c" "$here/ziiu_net.h" "$out/src/drivers/"
make -C "$out/hostapd" CC=gcc -j8 hostapd
"$out/hostapd/hostapd.exe" -v 2>&1 | head -1
