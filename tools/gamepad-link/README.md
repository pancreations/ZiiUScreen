# GamePad connection development

This is the Windows PC side of the saved-console connection, using the dedicated
5 GHz radio backend and the locally saved GamePad PMK. It is under hardware validation.
It currently implements directed probe replies, open authentication, association,
and the Nintendo WPA2 four-way handshake. Stale-session data from the saved
GamePad triggers a rate-limited reason-7 deauthentication response so the peer
can start fresh authentication. It does **not** yet install encryption
keys or provide DHCP, UDP input, audio or video. A verified handshake is only a
milestone, not a usable GamePad connection.

Driver 0.5.3 passes the PC completion check (one queued, one released, one status
report). Firmware emits version-5 TX-free notices and unified MCU TX status;
older builds missed those reports. Peer authentication remains unverified.
The worker uses nonblocking receive reads with short user-mode sleeps: the
driver's repeated 1 ms sleeps measured ~314 ms for an empty 20 ms wait on this
PC. Timing counters and send IOCTL duration remain in private diagnostics.

`ziiuctl ap-tx-check 36 5` runs a bounded, low-power AP and queues one directed
probe response to the saved GamePad. It requires driver 0.5.2+ for optional RX
diagnostics; use 0.5.3+ for this firmware's version-5 TX-free notices.
No GamePad action is required: token release is expected even when
there is no ACK. The event log includes DMA indexes, queued/released counts,
TX status, RX packet-type counts and MCU event-ID counts. These counters contain
no keys or packet contents. Raw RX capture remains private under `.local`.

`ziiuctl link-check` checks driver interface 5, inherited-handle transport and
saved credentials without starting the radio. `ziiuctl ap-connect 157 60` sets up
the hidden 3 dBm AP, launches the bounded worker and stops DMA on exit. The real
Wii U must be powered off first, since both use the same identity. Keep the
GamePad off until `.local/gamepad-link/connection-events.jsonl` reports `ready`,
then power on the GamePad. Logs contain state transitions, never key material.
One run is capped at 60 seconds after setup. The parent stops a stalled worker
and resets the radio. Forced termination of the parent still requires elevated
`ziiuctl dma-stop`, as with existing diagnostic AP commands.

The driver owns TX descriptors, tokens and every DMA address. Its 16 payload
slots stay pinned until a validated firmware TX-free notice releases them.
Advancing the DMA index is not enough to reuse payload memory. TX is available
only through the existing crash-latched IOCTL path and never at boot.

Offline checks:

```powershell
py -3 -m unittest discover -s tools/gamepad-link -v
cmd /c tools\gamepad-link\test-native.cmd
```

Python needs `cryptography` for AES key wrap (already available on this PC).
The remaining protocol operations use the standard library. Tests cover
Nintendo PTK rotation, authenticated messages 2/4, replay rejection, GTK wrapping,
station filtering, management sequencing, RX bounds, and truncated TX-free
notices. They do not establish hardware TX/RX correctness.

References: drc-hostap `CONFIG_TENDONIN` at
`418e5e206786de2482864a0ec3a59742a33b6623` (BSD), and the local mt76 source for
the reference backend's TXD/TXP, unified station setup and TX-free format (BSD-3-Clause-Clear).
These are protocol/layout references; the implementations here are separate.
See [REFERENCES.md](../../docs/REFERENCES.md).
