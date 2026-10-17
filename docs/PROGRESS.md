# Public snapshot status

This is an experimental source snapshot, not a supported release. Development
demonstrated the dedicated 5 GHz radio link and desktop streaming on a test setup.
The tree includes video/audio, touch/controller forwarding, rumble/IR controls,
a WPF launcher, and an experimental virtual display driver. Inclusion does not
mean that every feature is validated on new hardware.

Fresh setup still requires locally obtained pairing data from an already-modded
Wii U and its paired GamePad, local builds, and manual machine configuration.
Direct first-time pairing, robust reconnection, broad adapter support, and a
fully validated virtual-display installation remain unfinished. The Python
`gamepad-link` handshake prototype is separate from the hostapd streaming path.

The publication preparation used static checks and host tests; it did not install
drivers, change boot settings, transmit radio traffic, or validate a fresh PC.
Private development logs and hardware-session evidence are not published.
See [SETUP.md](SETUP.md) and [security limits](../SECURITY.md).
