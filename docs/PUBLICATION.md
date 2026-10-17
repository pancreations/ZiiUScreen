# Public snapshot and exclusions

This publication is a new source snapshot. It intentionally does not include the
private development repository's Git history, author/committer metadata, reflogs,
remote configuration, or deleted historical files.

## Excluded from the public copy

- All `.local/` contents: pairing exports, generated hostapd configuration,
  connection settings/tokens, console files, packet captures, screenshots,
  recordings, logs, dumps, cached third-party checkouts/toolchains, and backups.
- Build output, executables, driver packages, firmware, signing certificates,
  private keys, and generated homebrew binaries.
- `.vscode/`, personal agent handoffs, the original project prompt, private
  development journals, machine inventory/security-state tables, and local paths.
- Private hardware-session evidence and installed-binary hashes from development
  notes; concise technical status and recovery information replace those notes.

The public README, setup guide, agent instructions, and status summaries replace
private documents. Personal paths are replaced with relative paths. Numeric
protocol constants, generic device IDs, synthetic test data, upstream revision
pins and dependency integrity hashes remain because they are technical data.
Public third-party copyright notices remain intact. Hardware identifiers needed
by the implementation remain in source; they describe the reference backend and
are not published as the maintainer's personal hardware inventory. The source can
still reveal which chipset that backend targets; this is not a guarantee that
readers cannot infer hardware used during development.

Display-selection defaults in the publication copy are blank and must be chosen
locally. The stream entry point requires an explicit display and channel before
starting; the reset helper also requires these inputs. Original local settings
and the private development repository are preserved separately.

## Review scope

Preparation included source/document pattern scans, comparison against locally
saved pairing identifiers/keys, inspection of every reachable historical file
version, and review of network listeners, credential paths, and third-party
notices. The original history is excluded regardless of scan results.

This is not a penetration test, a hardware validation, or a legal clearance.
No scan can prove absence of all secrets or vulnerabilities. Public GitHub account
ownership remains visible. Do not upload private files or history in future updates.

## Legal context

The license and README disclaim warranties and liability only to the extent
applicable law permits. They do not guarantee immunity. Copyright and
circumvention restrictions and their exceptions depend on the jurisdiction and
facts; a modded console or personal-use label is not blanket permission.
See the [U.S. Copyright Office's Section 1201 materials](https://www.copyright.gov/1201/2024/)
for U.S. context. Obtain qualified legal advice for a determination about a release.
