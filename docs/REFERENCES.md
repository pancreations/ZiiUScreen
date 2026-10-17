# References

Prior work this project builds on. Check each project's license before reusing code;
anything copied or linked into ZiiUScreen must be license-compatible and credited.

Terminology used across these projects:

- **DRC** â€“ the GamePad ("Display Remote Controller").
- **DRH** â€“ the console side of the link (the Wii U's GamePad radio/host chip).
  For ZiiUScreen, the PC plays the DRH role.

## Protocol / reverse-engineering projects

| Project | What it is | Relevance | License |
|---|---|---|---|
| [libdrc](https://libdrc.org/) ([Wi-Fi RE notes](https://libdrc.org/docs/re/wifi.html)), source [bitbucket memahaxx/libdrc](https://bitbucket.org/memahaxx/libdrc) pinned at `5c26ba6d45920c8609466d00994e2bb313325ba9` in `.local/refs/libdrc` | memahaxx's C++ library for driving a GamePad from a PC (PC acts as the console). | Primary protocol reference. `tools/ziiu-drc` ports its console side (vstrm/astrm packets, command protocol, H.264 settings, input layout, send timing) with attribution in the file header. | Simplified BSD (2-clause), Copyright (c) 2013 Mema Hacking |
| [drc-x264](https://bitbucket.org/memahaxx/drc-x264) pinned at `998dacc19ff5b56d86f4b185011b89d2e80ab5e7` in `.local/refs/drc-x264` | x264 with libdrc's `b_drh_mode` (macroblock-row chunks, no slice header, no planar prediction) for the GamePad's decoder. | Built by `tools/ziiu-drc/build.sh` into `.local/build/x264-install` (not committed) and linked statically into `ziiu-drc.exe`, so redistributing that combined binary requires GPL compliance and corresponding source; no binaries are included. | GPL-2.0 (x264) |
| [libdrc-streaming](https://github.com/ywh233/libdrc-streaming) | Experimental libdrc fork streaming a Windows PC through a Raspberry Pi. | Confirms the GamePad freezes when the Wi-Fi TSF used for video timestamps is wrong or drifting. Read only. | See repo |
| [drc-hostap (rolandoislas fork)](https://github.com/rolandoislas/drc-hostap) â€“ see [README.DRC](https://github.com/rolandoislas/drc-hostap/blob/master/README.DRC) | hostapd/wpa_supplicant with `CONFIG_TENDONIN` patches for the GamePad's Wi-Fi changes. | Ships `wiiu_ap_normal.conf` (AP a GamePad connects to) and `wiiu_ap_pairing.conf` (WPS pairing). This is the AP side we need. | BSD (hostap upstream) â€“ verify fork |
| [hostap-drc (Daniel Foster)](https://zff.dev/daniel_foster/hostap-drc) | Same DRC patches rebased on recent hostap (2024). | Preferred base: newer hostap, newer kernels/drivers. | BSD (hostap upstream) â€“ verify |
| [drc-sim](https://github.com/rolandoislas/drc-sim) | Python backend that acts as a GamePad *to* a Wii U console. | Opposite direction from us, but useful for packet formats and as a working codebase to read. | TBD |
| [Vanilla](https://github.com/vanilla-wiiu/vanilla) | Maintained GamePad emulator (Linux, Steam Deck, Android, Switch). Also the opposite direction (PC acts as GamePad). | Most active community; the [Wireless-Compatibility](https://github.com/vanilla-wiiu/vanilla/wiki/Wireless-Compatibility) list and the ["pipe" design](https://github.com/vanilla-wiiu/vanilla/wiki/Pipe) apply to us directly. | GPL-2.0-only (per AUR listing â€“ verify upstream) |
| [WiiUBrew â€“ GamePad](https://wiiubrew.org/wiki/GamePad) | Community hardware/protocol wiki. | Hardware details, sensors, UIC, etc. | Wiki content |
| [drc-wireshark](https://bitbucket.org/memahaxx/drc-wireshark) | Wireshark with Nintendo cipher-suite support. | Capturing and decoding traffic while debugging. | GPL (Wireshark) |

## Local protocol study

Protocol research uses locally generated observations on authorized hardware.
Captures, pairing keys, console firmware, and private session evidence are not
distributed. The x264 patch is provided under upstream GPL terms; see
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Chip driver references

`tools/hostapd-win` builds the same pinned drc-hostap revision as a native Windows
`hostapd.exe` (BSD license) from `.local/refs/drc-hostap`; only a 59-line Windows
build patch and the build config are kept in this repo.

`tools/gamepad-link` uses the Nintendo RSN selectors and three-byte PTK rotation
documented by `CONFIG_TENDONIN` in the locally pinned drc-hostap revision
`418e5e206786de2482864a0ec3a59742a33b6623`. It is an independent Python WPA2
authenticator, using `cryptography` for AES key wrap. Driver interface 5's TXD,
hardware TXP and TX-free layouts were checked against the local mt76
chip-specific MAC/PCI source and shared `mt76_connac_mac.c` and
`mt76_connac3_mac.h`. TX status (TXS) fields follow `mt76_connac3_mac.h`. The TSF
read uses mt7996's connac3 LPON layout (`mt7996/regs.h`, `__mt7996_get_tsf`), not
`mt792x_get_tsf`, whose UTTR registers read 0 in the reference backend (checked with
`ziiuctl tsf-probe`). The 802.11 payload remains pinned until TX-free, separately
from descriptor DMA completion. Build/tests are not evidence of radio success.

| Project | Relevance | License |
|---|---|---|
| [mt76](https://github.com/openwrt/mt76) (chip-specific and shared transport code) | Hardware/firmware interface spec for our Windows driver: PCIe/DMA, firmware load, MCU commands, AP/beacon offload. | BSD-3-Clause / BSD-3-Clause-Clear |
| [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) vendor firmware tree | Firmware images selected by the reference fetch script. Fetched by `tools/fetch-firmware.ps1`, pinned to tag `20260916` with SHA-256 hashes; not committed. | `LICENSES/LICENCE.mediatek`: use and redistribution allowed with MediaTek chipsets, not under (L)GPL terms |

## Tooling

The standalone `tools/wiiu-workbench` Aroma plugin uses WiiUPluginSystem and
libmappedmemory (LGPL-3.0) and the GX2 capture technique from ScreenshotWUPS
(GPL-3.0). Exact revisions, upstream links and its GPL-3.0-or-later license are in
[its README](../tools/wiiu-workbench/README.md#sources-and-licensing). These sources
are separate from the Windows driver.

| Tool | Use | License |
|---|---|---|
| [dotnet-install.ps1](https://learn.microsoft.com/dotnet/core/tools/dotnet-install-script) + .NET 9 SDK | Downloaded by `tools/ziiu-app/build.ps1` into `.local/` (not committed) to build the desktop app. The app uses only WPF/WinForms from the .NET runtime, no NuGet packages. | MIT (.NET, the script) |
| [REA](https://github.com/morluto/rea) | MCP server for agent-driven reverse engineering (Ghidra/IDA/Hopper backends, pcaps). Fallback only, if we have to inspect MediaTek's Windows driver (`mtkwecx.sys`) where mt76 doesn't explain something. Needs Node 22+ and Ghidra (Windows support marked experimental). | MIT |

## Upstreaming discussion

- hostap mailing list, "Wii U support in upstream hostap":
  [Nov 2024](https://lists.infradead.org/pipermail/hostap/2024-November/043127.html),
  [Dec 2024](https://lists.infradead.org/pipermail/hostap/2024-December/043221.html),
  [Jan 2025](https://lists.infradead.org/pipermail/hostap/2025-January/043262.html).
  The maintainer suggested a per-network option for the modified PTK derivation. Check
  whether this landed upstream; if it did, we can drop the fork.

## Wi-Fi protocol differences (summary from libdrc RE notes)

The GamePad uses 5 GHz 802.11n with these deliberate deviations from standard Wi-Fi:

1. **Pairing (WPS)**
   - Console exposes an *open* AP, SSID `WiiU<mac2><mac>_STA1`.
   - Modified WPS: AuthKey = KDF(KDK rotated **3 bytes left**) instead of KDF(KDK).
     Standard WPS fails at M2.
   - PIN = 4 digits from card symbols (â™ =0 â™¥=1 â™¦=2 â™£=3) + fixed `5678`.
     When the PC is the host it chooses the PIN; the user enters the matching symbols.
   - Result: SSID, BSSID and PSK, which the GamePad stores.
2. **Normal operation (WPA2-PSK)**
   - Empty SSID.
   - RSN pairwise/group cipher and AKM selectors use the **Nintendo OUI `a4:c0:e1`**
     instead of the standard `00:0f:ac`. A mismatch is rejected (error 0x000c).
   - PTK is computed normally and then **rotated 3 bytes left**.
   - After association the GamePad does DHCP. Defaults: host `192.168.1.10`,
     GamePad `192.168.1.11`.

## Applying the references to a different adapter

Use [ADAPTING_YOUR_ADAPTER.md](ADAPTING_YOUR_ADAPTER.md) to investigate your own
chipset and driver interfaces. The mt76 sources below informed the existing
backend; another chipset may require a different public driver reference.
Protocol references explain the shared GamePad behavior. They do not certify
each adapter or prove that a different model cannot be adapted.

Hardware-controlled versus software-controlled authentication affects which
implementation work is possible through a given interface. Inspect the actual
driver/firmware interface instead of treating a marketing label as a verdict.

## Additional components

The virtual display driver adapts Microsoft Windows-driver-samples IddCx at
revision `2dc3fd3a` under MS-PL. Controller forwarding builds ViGEmClient at
`b66d02d57e32cc8595369c53418b843e958649b4` under MIT. Full notices and component
scopes are in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
