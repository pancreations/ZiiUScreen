# Wireless architecture and limitations

The GamePad uses a modified 5 GHz 802.11n link. This project shows how to make a
PC's 5 GHz adapter serve that link by adapting its driver and implementing the
GamePad protocol. Start with your own adapter and follow the
[hardware adaptation walkthrough](ADAPTING_YOUR_ADAPTER.md).

The included driver is a concrete hardware backend, not a universal driver. For
another chipset, adapt the hardware-specific portions and validate them in stages.
A model difference means porting work needs assessment; it does not by itself
establish that the adapter cannot work. No router modifications are part of this
workflow, and the documentation does not identify the maintainer's personal network.

The driver owns hardware DMA and exposes an administrator-only device interface.
The native drc-hostap port supplies authentication and a user-mode bridge for
DHCP/ARP/UDP. The media and input programs communicate through loopback sockets.
The protocol addresses `192.168.1.10` and `192.168.1.11` are GamePad-link constants,
not the author's home LAN addresses. Do not configure your home router to use them
on the basis of this document.

Use a separate internet connection while this adapter serves the GamePad. Only
bind the exact Wi-Fi function your backend implements and has been checked against. Never change Bluetooth or Ethernet
drivers. Confirm their health after every driver change. Restore normal Wi-Fi
with `tools/wifi-mode.ps1 Normal` if anything fails.

## Boot safety measures and recovery

Demand-start configuration, restricted hardware access, and a persistent crash
latch reduce risk; they do not guarantee freedom from crashes or boot failures.
Do not bypass the latch, introduce hardware access in boot/PnP paths, or add an
automatic startup service as part of setup. Investigate a failed operation before
clearing its latch. See [SETUP.md](SETUP.md#recovery) for recovery steps.

## Radio configuration

Select a supported channel that your own device and local regulations permit.
The implementation does not provide a complete certified regulatory-domain/DFS
management system. A channel being accepted by a script is not permission to
transmit on it. Do not broaden channel lists or power limits to work around this.
Use only your own authorized pairing data and keep the real console powered off
while the PC uses the same link identity. See [REFERENCES.md](REFERENCES.md).
