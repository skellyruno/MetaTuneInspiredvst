# MetaTune Inspired — LyraTune DSP edition

This is the original **MetaTune** plugin and UI, now running the **LyraTune pitch-correction engine from the supplied ZIP**. The JUCE editor, orb, piano, preset/automation IDs and plugin identity remain in place.

## Active tuning signal path

```
MetaTune APVTS / UI (unchanged layout)
  -> input sanitation + input meters
  -> LyraTune TunerChain
        -> anti-aliased streaming YIN (octave continuity guard)
        -> pitch prediction, note stabilizer and vibrato analysis
        -> signed retune (-3..400 ms engine domain)
        -> stereo-linked period-synchronous delay-line shifter
        -> internal aligned dry/wet path
  -> existing MetaTune doubler macro + delayed reference piano
  -> MetaTune host-PDC-aligned global bypass
```

The older `RealtimeAutotune.h` PSOLA, fusion verifier and research DSP files remain in source for compatibility/provenance, but they are **not in the active tuned-audio signal path**. We intentionally do not run two independent shifters at once.

### Retune behavior

The existing Speed dial is still a 0–100 control and keeps the established mapping: **0 → 200 ms**, **30 → 30 ms**, **approximately 94 → 0 ms**, **100 → -3 ms**. Unlike the old engine, the signed negative range is passed directly to LyraTune's correction controller. Below zero, correction responds instantly and scale-target hysteresis shrinks toward zero, creating the user's preferred aggressive hard-snap effect. Precision Hard Tune requests the -3 ms controller profile.

Sustain uses LyraTune's note-age-dependent speed adjustment, rather than applying the old MetaTune sustain multiplier a second time.

The Note Stabilizer toggle and None/Short/Mid/Long selector map to Lyra's candidate-note confirmation. Snap Live keeps the same low-delay Lyra synthesis and selects slightly tighter note hysteresis. Both toggles keep their original UI names and stored IDs.

### Latency

LyraTune's delay-line shifter is pitch synchronous and has a prepared latency budget based on the **70 Hz floor**. At 48 kHz:

* **Low Latency on:** about 7.5 ms of nominal input-to-output delay.
* **Low Latency off:** about 9.5 ms, including 2 ms causal detector lookahead.
* **Snap Live:** same audio latency as Low Latency on, but a different note-switch controller.

The host's reported latency changes on the message thread and dry/bypass output is aligned to the corresponding Lyra delay. These are plugin transport figures, not DAW device round-trip latency; output pitch may have locally varying read-head age as an inherent property of the Lyra shifter.

### Original source

The engine code in `Source/LyraDSP` is copied directly from the user-supplied archive `tune demo that sounds better than metatune.zip` (LyraTune-main), whose embedded source revision matches `333stack/LyraTune` commit `5f611543a21e3cf9b7ac4bc06ed9f854cde45235`. The only functional modification to the imported source is anti-phase-safe stereo analysis handoff in `TunerChain.cpp/.h`, leaving the distinct left and right synthesis streams untouched.

The original DSP suite is preserved in `tests/LyraDSP`. It tests negative retune, octave changes, vibrato, stereo, range switching, formant accent, CPU safety and allocation-free callbacks.

### Build and tests

A JUCE 8 checkout and CMake 3.22+ are needed:

```powershell
cmake -S . -B build -DJUCE_PATH=C:/JUCE -DSKELLTUNE_BUILD_TESTS=ON
cmake --build build --config Release --target MetaTuneInspired_VST3 LyraOriginalDSPTests
ctest --test-dir build -C Release --output-on-failure
```

GitHub Actions runs the DSP suite on Linux and compiles the VST3 on Windows. **Final sound quality and behavior in FL Studio should also be auditioned with real vocals**, especially fast note changes, low voices, negative retune and stereo/bypass transitions.

### Licensing and distribution

Read `LICENSE` and `THIRD_PARTY_NOTICES.md` before distributing binaries. Legacy MetaTune DSP has AGPL-derived provenance; the supplied LyraTune archive did not include a LICENSE file. The source revision is recorded for provenance, **not** as a claim that importing a public repository automatically grants distribution rights.
