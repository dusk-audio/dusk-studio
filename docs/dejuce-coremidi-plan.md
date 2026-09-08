# Native CoreMIDI — first macOS device phase

Partial work for issue #298. CoreAudio, the JUCE audio-device module, Windows MIDI, and hardware sign-off remain separate work.

## Boundaries

The existing `IMidiInputBackend` / `IMidiOutputBackend` interfaces and `MidiInputClient` / `MidiOutputBank` own the application-facing routes. The new backend exposes factories behind those interfaces. Linux continues using ALSA-seq. `DUSKSTUDIO_NATIVE_COREMIDI=ON` selects CoreMIDI on macOS; it defaults to `OFF`. The JUCE fallback remains available when the option is off. Other platforms reject an explicit native CoreMIDI request.

Input packets use a fresh correlation between `mach_absolute_time()` and `backendClockMs()` for every callback. A zero CoreMIDI timestamp means current callback time. Complete messages retain their first-byte time across packet boundaries. Each source owns a fixed parser buffer sized for the existing collector ring. Oversized SysEx is discarded whole; realtime messages still pass. EOX outside SysEx clears pending/running status and is not emitted alone. Undefined F4/F5 statuses retain the existing one-byte treatment.

Input callback state is retained by CoreMIDI's block. An atomic admission gate permits one callback per source, rejects stopped or concurrent delivery, and fences admitted callbacks before port disposal. Restart creates fresh callback state. Disabling input ports preserves the current attachment and endpoint notifications; detaching fences both input delivery and notifications. The message thread owns configuration. A single process client survives backend creation/destruction because Apple warns against disposing and recreating the last client. Ports and endpoint subscriptions close normally; weak notification subscriptions cannot keep an input backend alive.

Native IDs use `coremidi:<signed unique ID>`. Migration reconstructs existing JUCE identifiers from endpoint/device relationships and connected external IDs, including space-joined device/endpoint IDs and comma-joined connections. An alias matching more than one endpoint is refused. The forward lookup recognizes both verified single-entity external-device formats: device UID in JUCE 8.0.4 and endpoint UID in the newer fork. The reverse Apple-only lookup selects from the fallback's actual current enumeration, with the same ambiguity refusal, rather than guessing which format that framework version emits. The native canonical ID remains independent of names and enumeration order. Legacy name-prefix comparisons decode Unicode codepoints and use the current locale's single-character uppercase mapping, matching the fallback; they deliberately avoid broader Unicode folding or normalization.

Output uses one bounded scheduling worker across all destinations. Each send dispatches already-due events before returning, preserving the intended direct panic/control ordering before route closure. Future events remain on the worker, which orders deadlines in the same clock domain and sends only due packets to CoreMIDI. Closing routes or destroying the backend discards its remaining pending records. It does not flush destination-wide output belonging to other clients. The byte-packet CoreMIDI API remains deliberate while the application content model carries MIDI 1 bytes; UMP/MIDI 2 is outside this phase.

## Verification

Portable Catch tests cover message framing, packet splits, realtime interleaving, bounded SysEx recovery, collector capacity, signed IDs, and clocks with different epochs.

Enable the separately registered macOS suite with `-DDUSKSTUDIO_TEST_COREMIDI_VIRTUAL=ON`; the default test run does not create virtual endpoints. The virtual-endpoint suite exercises native enumeration, input/output, source timestamps, fragmented SysEx, scheduling, independent destinations, route cancellation, callback teardown/restart, compound identifier migration, ambiguity refusal, and endpoint notifications. It creates only its own virtual sources/destinations and does not change global MIDI setup or open hardware endpoints.

Validation uses exact-commit macOS app compilation and the native virtual tests, Linux app/test build and CTest, the isolated Linux selftest, source review, the JUCE gate and diff check. The macOS validation configuration disables LV2; no GUI or audio app runtime is part of this evidence. Virtual endpoints have no external device/entity hierarchy, so the two external identifier forms are checked against the actual framework sources and portable synthetic cases. They have not been exercised against physical external devices. SDK-only builds and virtual fixtures do not establish hardware timing or full application behavior.

The implementation deliberately uses CoreMIDI's deprecated MIDI 1 byte-packet APIs and suppresses those deprecation diagnostics in its native translation unit. An independent strict-warning build with AddressSanitizer and UndefinedBehaviorSanitizer exercises the portable and virtual suites. A separate scratch probe compiles the fallback against the actual macOS JUCE 8.0.4 source and checks signed, compound, missing and ambiguous routes through both backends' migration methods, using only its own virtual endpoints. Unrelated dependency warnings are compared with the parent revision, including a single-file parent build that reproduces sfizz's existing `std::result_of` warnings after CMake cache reuse changes its language standard.

Owed to Marc's bench: physical MIDI input/output, MTC and MIDI Clock jitter, physical disconnect/reconnect, driver-specific naming/identity, sleep/wake, and real CoreAudio/device integration. This phase does not complete issue #298 or globally unlink a JUCE module.

## API evidence

- [MIDITimeStamp](https://developer.apple.com/documentation/coremidi/miditimestamp): host time comes from `mach_absolute_time()`.
- [MIDIPacket.timeStamp](https://developer.apple.com/documentation/coremidi/midipacket/timestamp): timestamps describe occurrence/playback time; zero means now.
- [MIDIClientDispose](https://developer.apple.com/documentation/coremidi/midiclientdispose(_:)): disposing the last client can prevent later client creation.
- [MIDIClientCreate](https://developer.apple.com/documentation/coremidi/midiclientcreate(_:_:_:_:)): notifications use the creating run loop.
- [MIDIInputPortCreateWithBlock](https://developer.apple.com/documentation/coremidi/midiinputportcreatewithblock(_:_:_:_:)): input blocks run on a CoreMIDI-owned high-priority thread.
- [MIDIPacketListAdd](https://developer.apple.com/documentation/coremidi/midipacketlistadd(_:_:_:_:_:_:)): a packet may contain partial SysEx; packet lists are limited to 65,536 bytes.
- [MIDIFlushOutput](https://developer.apple.com/documentation/coremidi/midiflushoutput(_:)): cancellation applies to the destination, so backend teardown avoids it.
