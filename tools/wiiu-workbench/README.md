# ZiiU Workbench

**Use only the repaired 0.1.2 build.** Live checks confirmed status, GamePad
screenshots, timed input delivery/expiry, and the Aroma off switch. Application
transitions, title launch and memory reads still need runtime validation. Versions
0.1.0 and 0.1.1 are quarantined. The original SDK mismatch was fixed with pinned
WUPS 0.9.0. The subsequent title-list overrun path was removed: titles now come
from FTP, without console MCP API calls. Input uses a nonblocking mailbox.
See [REPAIR.md](REPAIR.md) for the installed-binary evidence and regression tests.
The client refuses control commands against earlier builds. Automatic first-time
installation remains held until live validation is complete.

Recovery if needed: with the console powered off, use a PC to rename
`wiiu/environments/aroma/plugins/ziiu-workbench.wps` on the SD card to
an unused name ending in `.disabled`, then reinsert the card and boot. Keep your own recovery backup under a non-plugin extension.
Host tests and compilation do not establish boot safety.

A standard, optional Aroma plugin and a Python PC interface for console research.
It loads with Aroma and starts its server in each application, including the Wii U
Menu. No foreground probe app needs to stay open. FTPiiU supplies file access;
the Workbench plugin supplies live inspection and input on TCP 49426.

## Turn it off

Open Aroma's plugin menu with **L + D-pad Down + Minus**. Select **ZiiU Workbench**
and turn **Enable remote workbench (this session)** off. Activation is session-only;
every power-up starts disabled, regardless of old saved settings. Disabling stops
the server and releases injected input. The plugin also uses normal WUPS unload
hooks, so Aroma's plugin-list disable/unload functionality can stop it like other
plugins. FTPiiU is independent and retains its own setting.

To remove it completely, turn it off and delete
`SD:/wiiu/environments/aroma/plugins/ziiu-workbench.wps`, then restart. The required authentication
token is `SD:/wiiu/ziiu-workbench.token`; the WUPS setting is stored under the Aroma
plugin config directory. There are no NAND patches, PC services, or driver changes.

## Install and build

The repository's portable devkitPro toolchain is described in
[SETUP.md](../../docs/SETUP.md). On a fresh toolchain, install the two pinned SDK
libraries, then build (the automatic installer remains held during validation):

```powershell
powershell -ExecutionPolicy Bypass -File tools/wiiu-workbench/setup-sdk.ps1
powershell -ExecutionPolicy Bypass -File tools/wiiu-workbench/build.ps1
```

The installer, once released, provisions a random token, saves the connection under the gitignored
`.local/wiiu/workbench/connection.json`, backs up an existing plugin, uploads to a
staging filename, verifies SHA-256, preserves the previous remote file, renames,
and verifies again. An existing token
is reused. Full power off/on loads a newly installed plugin. Use a full power restart; soft reboot was unreliable during development.

Requirements: Aroma with WUPS 0.9.0 support and MemoryMappingModule, FTPiiU, and Python 3. No extra Python packages are needed.
The token protects Workbench commands on the LAN; neither its TCP protocol nor
FTP is encrypted. Keep the endpoint on the local network.

## Use from the workspace

```powershell
tools/wiiu.ps1 status
tools/wiiu.ps1 radio
tools/wiiu.ps1 ls /fs/vol/external01/wiiu/apps
tools/wiiu.ps1 cat /fs/vol/external01/example.txt
tools/wiiu.ps1 get /fs/vol/external01/example.bin example.bin
tools/wiiu.ps1 put .local/example.bin /fs/vol/external01/example.bin
tools/wiiu.ps1 screenshot tv
tools/wiiu.ps1 screenshot gamepad --name gamepad.png
tools/wiiu.ps1 input RIGHT --ms 150
tools/wiiu.ps1 input A
tools/wiiu.ps1 input --x 2048 --y 2048 --ms 150
tools/wiiu.ps1 input --sx 1000 --sy 0 --ms 300
tools/wiiu.ps1 release
tools/wiiu.ps1 titles
tools/wiiu.ps1 launch <16-digit-installed-title-id>
tools/wiiu.ps1 menu
tools/wiiu.ps1 symbol nsysccr CCRCDCSysGetDrhState
tools/wiiu.ps1 read <0x-address-returned-by-symbol> 128
```

Available buttons: A, B, X, Y, LEFT, RIGHT, UP, DOWN, L, R, ZL, ZR, PLUS,
MINUS, HOME, TV, LSTICK, RSTICK. Separate chords with `+` or `,`.
Input automatically expires after 20–2000 ms. Touch coordinates are **raw** VPAD
0–4095 values, not screenshot pixels; calibration may be needed for exact taps.
Left-stick coordinates are -1000 to 1000. Physical input remains active.

Captures, downloads, memory dumps and backups stay in `.local/wiiu/workbench`.
File uploads are limited to the SD card and back up existing files. Reads can
browse all storage exposed by the existing FTP service. Memory reads are capped
at 4096 bytes and validate mapped user code/data addresses before access. Export
lookup only inspects already-loaded modules. It never calls arbitrary functions.

## MCP

The same executable offers newline-delimited stdio MCP:

```powershell
py -3 tools/wiiu-workbench/workbench.py mcp
```

Register that command with an MCP client if desired. It exposes 14 tools:
`wiiu_status`, `wiiu_radio`, `wiiu_list_files`, `wiiu_read_file`, `wiiu_download`,
`wiiu_upload`, `wiiu_screenshot`, `wiiu_input`, `wiiu_release`, `wiiu_titles`,
`wiiu_launch`, `wiiu_menu`, `wiiu_symbol`, `wiiu_memory_read`. Screenshot replies
include an actual PNG image. CLI access works immediately without an MCP-client
configuration change; coding agents can use it through their shell tool.

## Boundaries and validation

- This is a reusable inspection/control interface, not unrestricted access to
  every hardware subsystem. It does not expose arbitrary execution, kernel/MMIO
  reads, NAND writes, pairing-key export, or over-the-air Wi-Fi packets.
- A physical connected GamePad and an application polling VPAD are required for
  injected input. The plugin does not emulate a radio-connected controller.
- Screenshots cover the common GX2 scan-buffer-copy path, RGBA8/sRGB and no MSAA,
  up to 1920x1080 source resolution. Images are downsampled to at most 640x480.
  Other render paths or formats produce an explicit error or a two-second timeout.
- The Aroma plugin page and HOME overlays can suspend access: close the page
  before sending PC commands. Other foreground transitions can also suspend access. Wii mode,
  power-off and early boot are outside Aroma. The plugin restarts its worker on
  the next Wii U application. It cannot turn an off console on remotely.
- The client makes one connection attempt per command and never blindly retries
  input or title launch. If a transition interrupts the response, inspect status
  once afterward; the command may already have been accepted.
- `radio` reads public nsysccr state queries. These cannot reveal a WPA handshake.
  Use the saved captures and protocol sources for that work.

Host-side tests:

```powershell
py -3 -m unittest discover -s tools/wiiu-workbench -v
```

They cover fragmented/truncated network replies, binary and record framing, MCP
dispatch, malformed requests, command/path validation, PNG encoding, and rejecting
corrupt uploads before replacing a file. A successful build and these tests do
not establish on-console input, screenshot or disable behavior; record live
results in [PROGRESS.md](../../docs/PROGRESS.md).

## Sources and licensing

This standalone tool is GPL-3.0-or-later; see LICENSE. It does not change the
licensing of the separate Windows driver. The GX2 screenshot technique follows
[ScreenshotWUPS](https://github.com/wiiu-env/ScreenshotWUPS), GPL-3.0, revision
`d6b2fc846ba09dae10611495122fa5e99adf3970` (Maschell and contributors).
SDK dependencies: [WiiUPluginSystem](https://github.com/wiiu-env/WiiUPluginSystem),
LGPL-3.0, revision `2c76941e9358eda6be86363474434c4db1af860e` (WUPS 0.9.0,
stored separately in `.local/refs/WiiUPluginSystem-compatible`), and
[libmappedmemory](https://github.com/wiiu-env/libmappedmemory), LGPL-3.0, revision
`8a2273bfa47d7c207bda81f3e823026c5be72f92`. Their source and licenses remain in
`.local/refs`; the setup script recreates them. API definitions come from the
installed devkitPro wut SDK. FTP lifecycle behavior was checked against
[FTPiiU](https://github.com/wiiu-env/ftpiiu_plugin).
