#!/bin/sh
# Build ziiu-drc (console-side GamePad services) with MSYS2 MinGW-w64.
# Needs drc-x264 (GPLv2, see docs/REFERENCES.md) from the pinned checkout in
# .local/refs/drc-x264, built with assembly (needs yasm), with drc-x264-vbv.patch:
# the fix for encode failures on VBV row retries (owner approved, 2026-10-15, after
# encode-failure bursts with resync storms on the unpatched baseline).
# ZIIU_X264_VBV_PATCH=0 builds the plain upstream pin (the original baseline recipe).
# Each variant has its own cache, rebuilt automatically when the pin or patch changes.
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -l tools/ziiu-drc/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src="$repo/.local/refs/drc-x264"
pin=998dacc19ff5b56d86f4b185011b89d2e80ab5e7
patch="$here/drc-x264-vbv.patch"
if [ "${ZIIU_X264_VBV_PATCH:-1}" = 1 ]; then
    patch_id=$(sha256sum "$patch" | cut -d ' ' -f1)
    x264="$repo/.local/build/x264-install"
else
    patch_id=upstream
    x264="$repo/.local/build/x264-install-upstream"
fi
build_id="$pin-$patch_id"

if [ ! -f "$x264/lib/libx264.a" ] || [ "$(cat "$x264/ziiu-build-id" 2>/dev/null || true)" != "$build_id" ]; then
    [ "$(git -C "$src" rev-parse HEAD)" = "$pin" ] || { echo "drc-x264 is not at $pin" >&2; exit 1; }
    build_src="$repo/.local/build/drc-x264-$patch_id"
    rm -rf "$build_src"
    mkdir -p "$build_src"
    git -C "$src" archive "$pin" | tar -x -C "$build_src"
    [ "$patch_id" = upstream ] || (cd "$repo" && git apply --directory=".local/build/drc-x264-$patch_id" "$patch")
    (cd "$build_src" &&
        ./configure --disable-cli --enable-static --disable-opencl --prefix="$x264" &&
        make -j8 && make install)
    printf '%s\n' "$build_id" > "$x264/ziiu-build-id"
fi
# ZIIU_DRC_OUT builds to another file, e.g. while the normal ziiu-drc.exe runs (locked).
out="${ZIIU_DRC_OUT:-$repo/.local/build/ziiu-drc/ziiu-drc.exe}"
mkdir -p "$(dirname "$out")"
g++ -O2 -std=c++17 -Wall -Wextra -I"$x264/include" "$here/ziiu_drc.cpp" \
    -o "$out" \
    -L"$x264/lib" -lx264 -ld3d11 -ldxgi -ld3dcompiler_47 -lws2_32 -lwinmm -lole32 -static
echo "built $out"
