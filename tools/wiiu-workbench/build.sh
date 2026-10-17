#!/usr/bin/env bash
# Invoked through portable MSYS2 on Windows by build.ps1.
set -euo pipefail
wups_root="$(cd ../../.local/refs/WiiUPluginSystem-compatible && pwd)"
make WUPS_ROOT="$wups_root"
