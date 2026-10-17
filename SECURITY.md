# Security limitations and handling private data

This experimental project has not received a comprehensive security audit. No
supported versions or security response time are promised. A publication scan is
not proof that all vulnerabilities or sensitive data have been found.

## Private by default

Keep `.local/`, pairing exports, SSIDs, MAC addresses, WPA keys, Workbench tokens,
connection settings, screenshots, recordings, packet captures, memory dumps,
logs, driver backups, signing certificates/private keys, and generated binaries
off public Git repositories. `.gitignore` is a guardrail, not access control: it
does not protect already-tracked files, Git history, or files uploaded manually.
Generated executables can embed the local checkout path. Build your own signing
material; never reuse or share the author's private files.

The console probe intentionally exports pairing data to `SD:/ziiu-probe.txt`.
Treat that entire file as a credential. Copy only to your local private workspace;
remove the SD export when no longer needed. Do not attach it to an issue or feed
its contents to a cloud agent. Workbench connection files can also contain secrets.

## Network and privilege boundaries

- Windows media, input, and radio bridge sockets bind to loopback. They are not
  authenticated against other local processes; use a trusted, single-user test
  machine. Never forward these ports.
- The optional Wii U Live Probe listens on TCP 49425 on the LAN without
  authentication or encryption. It is for read-only status research; close it
  when finished and do not use it on an untrusted network.
- The optional Workbench listens on TCP 49426 when enabled. It requires a token
  and starts disabled per session, but the transport is unencrypted and permits
  inspection/control operations. FTP is also unencrypted. Use an isolated trusted
  LAN, disable both services when unused, and never expose them to the internet.
- Local administrators can access the custom Wi-Fi driver. Test signing and
  certificate trust change the computer's security posture. Keep recovery media
  and relevant recovery keys available before making boot/driver changes.
- Virtual display shared memory is readable by interactive users. Do not stream
  sensitive desktop content on a machine with untrusted local users/processes.
- Build scripts fetch third-party tooling. Review downloads and upstream
  instructions. Not every downloaded tool is pinned or checksum-verified.

## Reporting

Do not post credentials, raw captures, dumps, or working exploit details in public
issues. Use the repository's private vulnerability-reporting channel if enabled;
otherwise ask the maintainer for a private channel using a non-sensitive summary.
No private reporting channel is promised by this file.

If a secret is ever published, remove the exposure and replace/re-pair the
affected credential as appropriate. Deleting a file or rewriting Git history
does not revoke a credential or erase copies already downloaded.
