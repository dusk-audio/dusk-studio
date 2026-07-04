# Security Policy

## Supported versions

Dusk Studio is distributed as source (GPL-3.0) and as unsigned precompiled
binaries to supporters. Security fixes land on `main` and in the next tagged
release. Only the latest release is supported — there is no back-porting to
older tags.

## Reporting a vulnerability

Please report suspected vulnerabilities privately. Do **not** open a public
issue for anything exploitable.

- Preferred: [GitHub private vulnerability reporting](https://github.com/dusk-audio/dusk-studio/security/advisories/new)
  (Security → Advisories → *Report a vulnerability*).
- Alternatively, DM via Patreon if you are a supporter, or use the support
  email included in the private releases-repo invite.

Include: affected version (`Dusk Studio --version`), platform, reproduction
steps, and a proof-of-concept file if relevant. Expect an initial response
within a week.

## Scope

Dusk Studio parses and executes untrusted third-party content. The relevant
attack surface:

- **Session files** (`session.json`) — deserialized on load and autosave.
- **Audio + sample files** — WAV/AIFF/FLAC/MP3 and SF2/SFZ/multisample
  bundles, decoded via JUCE format readers and the multisample loader.
- **Native plugins** — CLAP, LV2, and VST3 binaries are loaded into the
  process at runtime. Plugin *scanning* runs sandboxed in a separate process
  (`src/engine/ipc/`); plugin *hosting* runs in-process by default.
- **Project import** — Tascam DP-24SD project import (`DpImporter`).

Loading a plugin, session, or sample from an untrusted source runs that
third party's code or parser with your privileges. Treat plugin binaries and
session bundles from unknown origins as you would any untrusted executable.

## Out of scope

- Bugs requiring the attacker to already control the machine or the audio
  device.
- Crashes or undefined behaviour in third-party plugin binaries themselves —
  report those to the plugin vendor. Dusk Studio's sandboxed scanner is meant
  to contain scan-time crashes, not to fully isolate a hosted plugin.
- The unsigned nature of the distributed binaries. This is a deliberate,
  documented choice (see README); verify source builds yourself if you need a
  trust anchor.
