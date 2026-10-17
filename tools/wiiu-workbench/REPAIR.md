# Workbench repair and remaining limitations

Use the 0.1.2 source. Earlier builds are not supplied. Title enumeration through
console MCP APIs was removed because installed homebrew hooks can violate caller
buffer assumptions. Title browsing uses FTP; launch/input paths remain experimental.
The nonblocking input mailbox bounds reads and expires injected input.

The server requires a valid local token, starts disabled each session, and has an
Aroma off switch. Its protocol and FTP transport are unencrypted. Keep it on an
isolated trusted LAN and off when unused. Automatic first-time installation
remains held pending validation. Compilation and host tests do not prove boot safety.

For recovery, power off and rename the plugin on the SD card to a non-plugin
extension using a PC. Private installed-binary evidence and console inventories
are excluded. See README.md and ../../SECURITY.md.
