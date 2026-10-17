# Agent-assisted setup and recovery

Start with the [README](../README.md), [agent instructions](../AGENTS.md),
[security limitations](../SECURITY.md), and [license notices](../THIRD_PARTY_NOTICES.md).
This is a manual research workflow, not a promise of a working fresh installation.
An agent should inspect your local environment and explain each privileged change.
Do not give an agent unrestricted access to secrets or blanket system permissions.

## 1. Inspect hardware before building or installing

```powershell
Get-PnpDevice -Class Net -PresentOnly |
  Format-List FriendlyName, InstanceId
```

Identify your Wi-Fi adapter's chipset, bus, driver, and 5 GHz capabilities locally.
Different models need their own hardware assessment and may need a driver port.
Follow [the adaptation walkthrough](ADAPTING_YOUR_ADAPTER.md) before installation;
it explains what we implemented and which parts to change for another model.
Keep hardware IDs and machine inventories local. No router model, router password,
or home-network configuration needs to be published.

Arrange Ethernet or another independent internet connection while dedicating the
adapter to the GamePad. Record Bluetooth/Ethernet health locally. The included
hardware checks prevent installing an unmatched driver; do not bypass them as a
substitute for implementing and validating your adapter's backend.

## 2. Prepare tools and upstream source

Use Windows 11 x64, Git, CMake, Python 3, Visual Studio C++ with Spectre libraries,
and a compatible Windows SDK/WDK. The development baseline used VS 2022 and
SDK/WDK 10.0.26100. Check current upstream documentation for your installation.
Native media/radio builds use MSYS2 MinGW-w64 GCC, make, and yasm. Optional Wii U
tools use devkitPro/wut; review their upstream setup and the local build scripts.

Clone this public repository into your chosen workspace. The following commands
build the reference implementation. For a different chipset, first adapt its
firmware fetcher, driver/backend, and device-switching scripts as described in the
walkthrough. Use only firmware intended for that chipset. From the repo root:

```powershell
New-Item -ItemType Directory -Force .local/refs | Out-Null
git clone https://github.com/rolandoislas/drc-hostap .local/refs/drc-hostap
git -C .local/refs/drc-hostap checkout 418e5e206786de2482864a0ec3a59742a33b6623
git clone https://bitbucket.org/memahaxx/drc-x264 .local/refs/drc-x264
git -C .local/refs/drc-x264 checkout 998dacc19ff5b56d86f4b185011b89d2e80ab5e7
powershell -File tools/fetch-firmware.ps1
powershell -File driver/build.ps1
cmake -S tools/ziiuctl -B build/ziiuctl -G 'Visual Studio 17 2022' -A x64
cmake --build build/ziiuctl --config Release
```

Review the downloaded firmware license; keep firmware local. Do not bypass a
failed hash check. In an MSYS2 **MinGW64 shell opened at this repository root**:

```sh
sh tools/hostapd-win/build.sh
sh tools/ziiu-drc/build.sh
```

Optional controller forwarding uses the ViGEmClient revision specified in
`tools/ziiu-input/CMakeLists.txt` and requires a separately installed compatible
ViGEmBus. Build with CMake after reviewing that dependency. The WPF app builds
with `powershell -File tools/ziiu-app/build.ps1`; it downloads a local .NET 9 SDK
and requires the .NET 9 Desktop Runtime. Review the download before execution.
The virtual display driver is experimental; `driver/build-display.ps1` builds it,
but a universally validated installation procedure is not provided.

## 3. Obtain your own pairing data locally

This workflow requires your own already-modded Wii U (Aroma), its paired GamePad,
and the Wii U homebrew toolchain. It does not include console-modding instructions,
firmware, console keys, games, or a first-time pairing wizard.

Build `tools/wiiu-probe` using its build script after setting up devkitPro/wut.
Review the script's toolchain path assumptions. Run the resulting homebrew on
your own console. It writes `SD:/ziiu-probe.txt`, which contains pairing secrets.
Store it privately at `.local/wiiu/ziiu-probe.txt`; do not post it or paste it into
an agent chat. Remove the SD export when no longer needed. If using FTP for the
transfer, use an isolated trusted LAN and disable FTP afterward.

The [Workbench](../tools/wiiu-workbench/README.md) and
[Live Probe](../tools/wiiu-live-probe/README.md) are optional research helpers;
they are not required to stream. Their servers have the limitations in SECURITY.md.
Workbench's automatic first-time installer remains held pending validation.

## 4. Driver installation is a deliberate user action

After confirming the device-switching script matches your implemented backend,
back up your normal adapter driver with `tools/wifi-mode.ps1 Backup` and retain
recovery access and any disk-encryption recovery key privately. The custom driver
is test-signed. Enabling Windows test signing may require disabling Secure Boot;
that reduces boot protections and may conflict with anti-cheat or device policy.
Do not change these settings silently or on a managed PC without authorization.

If you choose to proceed, follow current Windows/WDK instructions to configure
test signing, reboot as needed, and review `tools/trust-test-cert.ps1` before
trusting the certificate you built. No developer certificate is supplied.
An agent must explain the effects of boot or trust changes before you perform them.

```powershell
powershell -File tools/wifi-mode.ps1 GamePad
powershell -File tools/wifi-mode.ps1 Status
```

Confirm Bluetooth works with a paired device and the independent internet link
still works. Restore `Normal` mode immediately if either fails.

## 5. Choose your display and permitted 5 GHz channel

Use Windows display settings and the app's display list to identify the output
you intend to capture. A physical dummy display is one option; the virtual driver
is experimental. Select only content you intend to show. There is no personal
display default in the publication copy. Choose a channel supported by your own
GamePad and permitted locally; the script's allowed numbers are not a regional
regulatory check. Do not transmit if uncertain about channel/power restrictions.

With the real console off and the GamePad ready, start from an elevated PowerShell
using values you have chosen (replace placeholders):

```powershell
powershell -File tools/ziiu-drc/run.ps1 -Capture '<your-display-id>' -Channel <your-channel> -Seconds 30
```

A short diagnostic is useful before continuous use. Omit `-Seconds` for a session
that runs until stopped. Select audio routing explicitly if the default output
contains private material. Configure local controller/touch calibration as needed.

```powershell
powershell -File tools/ziiu-drc/run.ps1 -Stop
powershell -File tools/wifi-mode.ps1 Normal
```

The app stores local display/channel settings under `.local/app/`. The reset
helper requires your display and channel too. `-PreFeatures` refers to a private
historical encoder binary that is intentionally absent; use the source build.

## Recovery

Stop the stream and restore `tools/wifi-mode.ps1 Normal` when Windows is working.
After a crash, keep dumps private and investigate before using `ziiuctl clear-latch`.
The crash latch is a mitigation, not a guarantee of boot safety.

If Windows fails to boot, use Windows Recovery / Startup Settings to enter Safe
Mode. From an administrator prompt, disable the project Wi-Fi service:

```powershell
reg add HKLM\SYSTEM\CurrentControlSet\Services\ziiu_wifi /v Start /t REG_DWORD /d 4 /f
```

Restart and restore the normal vendor driver. If Safe Mode is unavailable, use
Windows recovery media and qualified assistance to disable the service in the
offline SYSTEM hive; verify the actual Windows volume and selected control set
instead of assuming a drive letter or `ControlSet001`.

For full removal, restore Normal mode first, identify only the ZiiUScreen package
with `pnputil /enum-drivers`, remove that package, and review
`tools/trust-test-cert.ps1 -Remove`. After all test drivers are removed, restore
your original boot/security settings. Do not delete unrelated driver packages.

For a Wii U plugin boot problem, power off, remove the SD card, rename the affected
plugin to a non-plugin extension on a PC, and restart. Keep recovery backups local.
