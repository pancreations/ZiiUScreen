# ZiiUScreen

An experimental personal project exploring how to use an original Wii U GamePad
as a Windows display, audio output, touchscreen, and controller. This repository
shares source code and research methods so other owners can recreate and adapt
the experiments with their own GamePad and their own modded Wii U.

**Use a coding agent to help adapt the setup to your own hardware and 5 GHz
environment.** Start the agent with [AGENTS.md](AGENTS.md) and
[docs/SETUP.md](docs/SETUP.md). Have it inspect the actual adapter, display, tools,
and local configuration before suggesting changes. An agent is an aid, not a
compatibility guarantee; review privileged commands and keep pairing secrets out
of chats and public issues.

## What to expect

- This is a source-and-methods snapshot of a personal experiment, not a supported
  product or a one-click installer. There is no support, maintenance, security
  update, or response-time commitment.
- Start with a **5 GHz Wi-Fi adapter** and use an agent to adapt the radio/driver
  setup to your own model. We made the PC act as the GamePad's host by implementing
  the required driver access, authentication, packet transport, and timing. Follow
  [how we did it and how to adapt it](docs/ADAPTING_YOUR_ADAPTER.md).
  Different models may need different driver work; they are not ruled out just
  because they differ from the reference implementation. Compatibility is a
  result to establish by implementation and testing, not a promise for every model.
  This process works on the PC's wireless adapter; no home-router modification
  is part of the method.
- The adapter is unavailable for ordinary Wi-Fi while this driver is bound.
  Arrange a separate internet connection. Do not modify Bluetooth or Ethernet
  drivers to make this work.
- The documented workflow uses pairing data obtained locally from your own
  already-modded console and GamePad. A fresh, console-free pairing wizard is
  unfinished. The console can be off during PC streaming after this setup.
- Channel, permitted transmit power, region, interference, display selection,
  audio routing, and calibration differ between setups. Numeric defaults are
  development examples, not a regulatory or compatibility determination.
- Test signing, administrator access, and driver installation are involved.
  Changing Secure Boot or other protections can affect security, recovery, and
  anti-cheat compatibility. Driver faults can crash or disrupt a computer.
- Video/audio/input features have been exercised during development, but the
  public snapshot has not been validated on a fresh PC. Reconnect behavior,
  latency, virtual-display behavior, and hardware compatibility remain experimental.
  Optional Wii U research plugins have additional limitations and recovery steps.
- No firmware, games, console dumps, pairing keys, signed drivers, executables,
  or personal captures are included. Build locally and obtain dependencies from
  their respective upstream projects.

## Start here

Read the [adapter adaptation walkthrough](docs/ADAPTING_YOUR_ADAPTER.md),
[setup and recovery guide](docs/SETUP.md),
[security limitations](SECURITY.md), [third-party notices](THIRD_PARTY_NOTICES.md),
and [publication exclusions](docs/PUBLICATION.md) before running anything.
Use only devices and networks you own or are authorized to work on. Keep the
real console powered off while the PC uses its saved GamePad link identity.

## Project notice

This is an independent hobby project, unaffiliated with and not endorsed by
Nintendo, Microsoft, MediaTek, or other referenced vendors. Product names and
trademarks belong to their respective owners. No trademark rights are granted.

The software and methods are provided **AS IS, without warranty**, subject to
the terms of the applicable licenses. To the extent permitted by applicable law,
the authors and contributors disclaim liability for use of this project. This
notice does not override third-party licenses or rights that cannot legally be
excluded, and is not a promise of legal immunity. Ownership of hardware does not
automatically resolve copyright, circumvention, radio, or other legal obligations.

Original contributions are under the [MIT license](LICENSE). Third-party code,
adaptations, and patches retain the licenses explained in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
