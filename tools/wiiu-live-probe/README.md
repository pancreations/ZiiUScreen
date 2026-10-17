# ZiiU Live Probe

This foreground Wii U app exposes read-only DRH and GamePad state on TCP port 49425
while it is open. Press HOME to stop it. It does not export EEPROM, SSID, MAC
addresses, or the WPA2 key. It does not change the radio's state.

Build with `powershell tools\wiiu-live-probe\build.ps1`. Install the resulting
`ziiu-live-probe.wuhb` under `SD:/wiiu/apps/` using the existing FTPiiU service,
then make a full power restart so Aroma refreshes the Wii U Menu. Start the separate
**ZiiU Live Probe** icon; the older **ZiiU Probe** is a different app. The owner's
soft reboot got stuck on the menu loading screen, so use a full power restart.

Direct PC query:

```
py -3 tools\wiiu-live-probe\bridge.py --status --host <Wii-U-IP>
```

For a stdio MCP client, start `py -3 tools\wiiu-live-probe\bridge.py --mcp --host
<Wii-U-IP>`. It exposes the `wiiu_radio_status` read-only tool. The console must
be on, connected to the LAN, and running the app. The MCP bridge uses only Python's
standard library and does not need an internet connection.

The status API provides connection state and query return codes. It cannot expose
the 802.11 authentication or WPA exchange; capturing those still requires a focused
radio capture during a real GamePad reconnect.
