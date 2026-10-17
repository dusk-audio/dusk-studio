# Released session fixtures

`v0.15.1-format10.json` is the unchanged output of the released Linux x86_64
Dusk Studio 0.15.1 binary, created on 2026-10-16. It is not a hand-written
approximation of format 10.

Provenance:

- Source tag: `v0.15.1`, commit `07bee15c242fa69639841e3a4fbe012caa05d663`.
- Release: `dusk-audio/dusk-studio-releases`, tag `v0.15.1`.
- Asset: `dusk-studio-0.15.1-Linux-x86_64.tar.xz`.
- Archive SHA-256: `e85a034bb669620212e57deac7ea2ee04621dbf9e092a6245a6b9435e19a252f`.
- Executable SHA-256: `61134795e028d044f88c2c84b471e39b9a0ee974e94baf4c55f4d704b08c7985`.
- Fixture SHA-256: `170fe21a5e9b8aefe87867bbd697aacd736bb497a9c39555646d3d0c35f15459`.

The archive matched the release's `SHA256SUMS`. `gpgv` verified
`SHA256SUMS.asc` with the key from that source tag, fingerprint
`5F80A0053B2F7481DF0A303477A891C01B55C206`.

To reproduce, extract the whole archive and run its existing
`gui:gui.quit_save_stamps_rate` helper through the repository's private Xvfb
and PipeWire null-audio harness. Use fresh HOME, config, runtime and music
directories. Clear inherited `DUSKSTUDIO_LOAD_SESSION`, `DUSKSTUDIO_CAPTURE_DIR`,
`DUSKSTUDIO_QUIT_AFTER_MS`, `DUSKSTUDIO_START_STAGE` and `PIPEWIRE_REMOTE`.
The helper changes track 0's fader to -3 dB and drives actual Quit, Save and
Save As actions. Copy `music/Quit save at 48000/session.json` without editing
or reformatting it. The process must exit successfully with no FAIL, SKIP or
DIRTY marker and the shutdown message `audio callback already detached (skipping)`.

This fixture contains 24 empty tracks and a 48 kHz session rate, with no audio,
MIDI, markers, mastering source or personal session data. It covers the actual
released writer's schema, the nondefault mixer value, rate and defaults for
fields introduced later. It does not supply released MIDI or marker coverage.
