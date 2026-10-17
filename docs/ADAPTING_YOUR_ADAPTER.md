# Adapting the method to your own 5 GHz Wi-Fi adapter

Use a coding agent to help reproduce the engineering process with your own PC,
5 GHz adapter, GamePad, and already-modded Wii U. This guide describes how the
project's link was built and what has to be adapted for a different model. It is
a porting workflow; no new hardware model is claimed tested by this document.

The work is in the PC driver and software. It does not involve modifying your
home router or publishing its model, password, network name, or address.

## What we built

We studied public chipset-driver code and GamePad protocol documentation, built a
reversible Windows radio driver, brought up firmware and packet transport, and
ported the modified hostapd authentication path to Windows. We then added software
encryption, DHCP/ARP/UDP bridging, and video/audio/input with GamePad-compatible
timing. Observations from an authorized console/GamePad link helped validate the
protocol. Private captures, device identities, and credentials are not included.

The GamePad side of that work can be reused across ports. Chip register addresses,
firmware commands, bus access, descriptors, and driver installation must match the
new adapter. A successful port may require substantial implementation, not just
a configuration edit. Whether an adapter exposes enough control is an engineering
question to investigate; neither a different model name nor a 5 GHz label alone
establishes the final result.

## 1. Identify the adapter and the interfaces you can use

Inspect the Wi-Fi adapter locally with Device Manager or:

```powershell
Get-PnpDevice -Class Net -PresentOnly | Format-List FriendlyName, InstanceId
netsh wlan show drivers
```

Record the chipset, PCIe/USB bus, current vendor driver, supported bands/modes,
and the exact Wi-Fi function. Keep this inventory local. Windows' reported modes
are an initial clue, not proof of the control available to a custom driver.
Find public vendor documentation or appropriately licensed driver source for the
actual chipset. The existing backend used mt76 as its hardware-interface reference;
use the appropriate reference for your model and retain its license notices.

Determine how to receive/transmit management and data frames, select a permitted
5 GHz channel, control AP behavior, and obtain radio timing. Determine where
authentication and encryption execute and whether the interface permits the
GamePad's protocol variants. Prefer an existing usable interface when available;
otherwise assess a custom backend. If access is missing, document exactly what is
missing and investigate alternatives rather than guessing register writes.

## 2. Establish backup, restoration, and a separate connection

We kept the normal vendor driver available and made switching to the experiment
reversible. `tools/wifi-mode.ps1` implements that pattern for the reference backend:
select one exact Wi-Fi device, back up its driver, bind the project driver, and
restore the vendor driver on request. Port the device selector, vendor INF/provider
names, backup/restore logic, and health checks for your hardware before using it.

Keep a separate internet path while the adapter serves the GamePad. Do not change
Bluetooth or Ethernet drivers. Record their health and confirm it after each
change. Review test-signing, certificate trust, and recovery with the user before
installing a custom driver; see [SETUP.md](SETUP.md#recovery).

## 3. Implement hardware bring-up before running the GamePad protocol

Start with a device that can be opened and queried without radio transmission.
The existing implementation separates privileged device access from user-mode
protocol work. Use these files as a map, not as interchangeable chip definitions:

| Area | Existing implementation | What a new backend must establish |
|---|---|---|
| Device lifecycle, bus access, receive/transmit buffers | `driver/ziiu_wifi/driver.c` and its register header | Correct bus/register access, ownership, reset, bounds, and cleanup for the actual chip; USB needs its own transport rather than PCIe DMA assumptions |
| Tool/driver contract | `driver/include/ziiu_wifi_ioctl.h` | Equivalent operations, validated lengths, restricted access, consistent interface version |
| TX descriptor layout | `driver/include/ziiu_tx_format.h` | Actual descriptor/status format and buffer lifetime; a DMA completion is not necessarily a peer ACK or permission to free a referenced payload |
| Firmware and radio configuration | `tools/ziiuctl/ziiuctl.cpp` | Firmware loading, MCU messaging, channel/power configuration, station records, AP setup, and radio timestamp access |
| Firmware acquisition | `tools/fetch-firmware.ps1` | Correct images for this chipset, license, pinned source, and verified hashes |
| Binding and restoration | Driver INF and `tools/wifi-mode.ps1` | Exact matching device and safe vendor-driver restoration |

We brought up firmware communication and receive paths before attempting a full
connection. Port initialization, queues, RX parsing, TX status/free handling, and
shutdown as a unit. Do not merely add another hardware ID to the INF: the driver
must understand that hardware's registers, firmware, and buffer formats first.
Preserve the persistent crash latch, bounded operations, and explicit cleanup.

Pass this stage only when firmware replies are valid, receive records are bounded,
transmit resources are released correctly, and stopping/restoring works. Keep
passive observations restricted to your authorized devices and captures private.

## 4. Bring up the host role and prove both transmit and receive

We built the pinned drc-hostap source with `tools/hostapd-win/build.sh`, applied
`windows.patch`, and supplied `driver_ziiu.c` as its Windows hardware backend.
This lets hostapd handle management and authentication while the driver transports
frames. Port hardware-specific station setup and transport calls in this backend
as well as those in `ziiuctl`; hardware assumptions exist in both places.

Check beacon transmission, directed probe reception/replies, authentication,
association/reassociation, and peer acknowledgements independently. In development,
beacons could transmit while the AP receive path delivered no useful frames.
Enabling the reference firmware's sniffer receive path alongside AP operation
resolved that issue. For another chip, find its supported way to deliver those
frames; copying that firmware command is not a portable fix.

Choose the user's locally permitted channel and power settings. Keep tests bounded
and verify shutdown actually stops transmission. Do not alter the home router.

## 5. Use your own console pairing data and validate authentication

The current method reuses pairing data from the user's own console/GamePad pair.
Build/run `tools/wiiu-probe` on that already-modded console, store its export in
`.local/wiiu/ziiu-probe.txt`, and keep the whole export private. Power off the real
console before the PC uses the same link identity. A fresh pairing wizard is
unfinished; this guide does not claim otherwise.

The modified hostapd path supplies the GamePad-specific RSN identifiers and key
derivation behavior documented by [libdrc](https://libdrc.org/docs/re/wifi.html).
Check the full four-message authentication exchange, not just association.
`tools/gamepad-link` contains offline frame/handshake tests and a separate Python
prototype; the streaming path uses the native hostapd backend.

## 6. Add encrypted packets and the network bridge

Association and a successful handshake did not yet provide media transport. We
added software CCMP in `driver_ziiu.c`, with authentication/replay checks and
offline self-checks, then DHCP, ARP, and UDP forwarding in `ziiu_net.c`.
Determine whether your receive path supplies encrypted or already-decrypted frames
and metadata; make encryption ownership explicit to avoid processing it twice.

Validate encrypted data in both directions, address assignment, ARP responses,
then input reports before video. Preserve peer filtering and packet bounds. The
bridge connects the dedicated radio link to loopback services on the PC; it does
not require exposing services through a home router. Firmware-specific station,
aggregation, and receive-filter behavior may need further adaptation here.

## 7. Align radio timing, then add video, audio, and controls

The streamer in `tools/ziiu-drc` uses libdrc protocol work and the pinned modified
x264 encoder. `ziiu_net.c` forwards radio timing samples. Implement and verify
your adapter's TSF/time source and its mapping to host time; a host timestamp or
an apparently working packet stream is not proof of correct GamePad timing.

Start with test-pattern video, then the user's chosen display, audio route, touch,
controller input, and rumble/IR. `tools/ziiu-input` handles Windows input forwarding.
Inspect latency at capture, encode, submission, completion, and playback. Our
debugging distinguished packet order, TX release timing, and background timer
behavior from encoder settings. Do not assume another adapter shares the same
timing or tuning values. Keep recordings and per-device calibration local.

## 8. Validate in stages and report the actual result

Run the existing offline checks before live testing:

```powershell
py -3 -m unittest discover -s tools/gamepad-link -v
py -3 -m unittest discover -s tools/wiiu-workbench -v
```

Native transport tests are in `tools/gamepad-link/test-native.cmd` and
`tools/hostapd-win/net_selftest.c`; the latter's build instructions are in its header.
`tools/ziiu-drc/check_stream.py` checks encoded protocol/video output with its
documented prerequisites. Extend the relevant transport tests for a new backend.

Record each milestone separately: driver load/restoration, firmware reply, receive,
transmit/ACK and cleanup, association, authentication, encrypted traffic, address
assignment/input, video/audio, then reconnect/stop/recovery. A build or host-test
pass proves only that check. Mark a new model as investigated, implemented, or
hardware-verified according to evidence; do not label it impossible just because
this repository does not yet implement its backend.

## Suggested task for your coding agent

> Read AGENTS.md, this adaptation guide, and SETUP.md. Inspect my 5 GHz adapter
> locally and identify its chipset, bus, driver interfaces, and public references.
> Map the reference backend to my hardware. Reuse the GamePad protocol work and
> implement the hardware-specific parts where needed. Explain privileged changes
> before installation. Validate one milestone at a time, preserve normal Wi-Fi,
> Bluetooth, Ethernet, and recovery, and keep all pairing/network data private.
> Report what is proven and what remains untested; a different model is a reason
> to investigate the port, not to assume failure or promise success.
