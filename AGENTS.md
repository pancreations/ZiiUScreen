# Working on ZiiUScreen

Read README.md, SECURITY.md, docs/SETUP.md, and THIRD_PARTY_NOTICES.md first.

- Treat this as experimental research. Do not claim compatibility or runtime
  success from compilation or host tests alone.
- Inspect the user's 5 GHz adapter and follow docs/ADAPTING_YOUR_ADAPTER.md.
  For a different model, research and implement the corresponding hardware backend;
  do not treat a model mismatch as proof that the project cannot work on it.
  Do not bind the existing driver to a different chipset by merely changing its
  hardware-ID checks. Match register, firmware, bus, and packet interfaces first.
- Do not identify reference hardware as the maintainer's personal equipment or
  publish personal router/network details. Keep required chipset identifiers in
  implementation code as technical compatibility data.
- Determine the user's own display, permitted 5 GHz channel, audio routing, and
  pairing configuration. Do not reuse the developer's machine defaults blindly.
- Keep credentials, console exports, captures, logs, and signing files in ignored
  local storage. Never print or upload pairing keys, tokens, or raw private files.
- Explain privileged changes before the user performs them. Do not silently alter
  Secure Boot, test signing, certificate trust, firewall rules, or router settings.
- Preserve Bluetooth, Ethernet, normal Wi-Fi restoration, and the radio crash
  latch. Do not clear a crash latch without investigating the failure.
- Use only the user's authorized hardware and networks. Do not operate the real
  console and PC with the same saved link identity at the same time.
- Optional Wii U research servers must remain off when unused. Never expose them
  or the loopback bridge ports to the internet.
- Preserve third-party notices. Original code is MIT; component licenses take
  precedence for the components identified in THIRD_PARTY_NOTICES.md.
- Publish only the audited export. Never push the original private development
  history or copy `.local`, build outputs, or editor settings into a public repo.
