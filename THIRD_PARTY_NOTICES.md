# Licensing and third-party notices

The root MIT license covers original ZiiUScreen contributions only. It does not
relicense third-party material. Retain copyright notices and applicable license
texts when copying, modifying, or redistributing components.

| Component / source | Applicable terms and included notices |
|---|---|
| Original project code and documentation, except the components below | [MIT](LICENSE), copyright 2026 pancreations |
| libdrc-derived protocol, streaming, and input portions | Copyright (c) 2013 Mema Hacking; Simplified BSD; [full upstream notice](LICENSES/libdrc.txt). See headers in `tools/ziiu-drc` and `tools/ziiu-input`. |
| mt76-derived register/interface definitions | Copyright (C) 2016 Felix Fietkau; Copyright (C) 2020-2023 MediaTek Inc. and contributors. File-level BSD-3-Clause-Clear notices take precedence over the repository BSD-3-Clause overview; [overview](LICENSES/mt76.txt), [Clear BSD terms](LICENSES/BSD-3-Clause-Clear.txt). |
| `driver/ziiu_display` (Microsoft IddCx sample adaptation) | Copyright (c) Microsoft Corporation; changes copyright 2026 pancreations. Distributed under [Microsoft Public License](driver/ziiu_display/LICENSE), including the original Microsoft notice. This directory is not relicensed MIT. |
| `tools/ziiu-drc/drc-x264-vbv.patch` and its drc-x264 target | GPL version 2 or later; [upstream GPL text](LICENSES/drc-x264-GPL-2.0.txt). The patch is supplied under those terms. Building the streamer statically links GPL x264: redistributing the resulting combined executable requires GPL compliance, including corresponding source; a root MIT notice alone is insufficient. |
| `tools/hostapd-win/windows.patch` and its drc-hostap target | BSD terms from the pinned upstream fork; copyright Jouni Malinen and contributors; [full upstream notice](LICENSES/drc-hostap.txt). |
| `tools/wiiu-workbench` | GPL-3.0-or-later, as already declared; [license](tools/wiiu-workbench/LICENSE) and [sources](tools/wiiu-workbench/README.md#sources-and-licensing). Separate from the Windows programs. |
| ViGEmClient, fetched separately for `tools/ziiu-input` | MIT, copyright 2018 Benjamin Hoeglinger-Stelzer (original spelling retained in [upstream notice](LICENSES/ViGEmClient.txt)). ViGEmBus is a separate runtime dependency; no binary is bundled. |

Upstream references and revision pins are in [docs/REFERENCES.md](docs/REFERENCES.md)
and the build scripts. License files copied into `LICENSES/` are notices, not
bundled implementations. Public upstream author attribution, including contact
details within their notices, is intentionally retained.

MediaTek firmware is fetched separately with pinned hashes by
`tools/fetch-firmware.ps1`; its vendor license applies. Nintendo/Broadcom console
firmware, games, proprietary SDKs, console dumps, and private pairing exports are
not distributed. No permission to distribute those materials is implied.

The .NET SDK, Windows SDK/WDK, MSYS2, devkitPro/wut, WUPS, libmappedmemory,
ScreenshotWUPS, and other tools/dependencies retain their upstream terms. This
source snapshot ships no executable release. Review the exact dependency versions
and corresponding-source obligations before distributing your own binaries.

These notices document the sources identified during preparation, not a legal
opinion or a guarantee that every provenance or patent question is resolved.
