# MetaTune Inspired — LyraTune DSP edition

This is the original **MetaTune** plugin and UI, now running the **LyraTune pitch-correction engine from the supplied ZIP**. The JUCE editor, orb, piano, preset/automation IDs and plugin identity remain in place.

## Active tuning signal path

```
MetaTune APVTS / UI (unchanged layout)
  -> input sanitation + input meters
  -> LyraTune TunerChain
        -> anti-aliased streaming YIN (octave continuity guard)
        -> pitch prediction, vibrato analysis and Skell Live note tracking
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

### Restored MetaTune automation controls

The existing JUCE parameter/automation IDs **glide**, **correctionLimit**, and
**consonantRelease** are forwarded into the live Lyra DSP and tested on actual
audio. Their original ranges and preset/state identifiers are unchanged.

* **Glide (0–500ms)** eases the *confirmed target note* after a transition
  within a phrase. It never slows down YIN detection or makes a new phrase
  slide from an old note. At 0ms, negative retune is the same instant snap as
  before.
* **Max Correction (0–12 semitones)** limits correction in the controller and
  ramps changes in the limit over 4ms, protecting automation against sharp
  sample-to-sample jumps. The Lyra **shifter** still has a physical
  ±approximately 7-semitone ratio envelope; values above it require a
  redesigned reconstruction path, and the output meter correctly reflects
  the real cap.
* **Consonant Release (0–30ms)** independently controls how the tuning ratio
  returns to unity on unvoiced or pending sound, rather than inheriting Retune
  Speed's old 20ms floor. Zero means the fastest **click-safe** response,
  with a 1ms minimum fade.

Regression tests include note-to-note versus phrase-onset glide, capped
correction under automation, independent consonant release, and real output
ratios from the production MetaTune/Lyra adapter across 44.1/48/96 kHz.


### Skell Live (replaces Snap Live)

The switch in the MetaTune selector bar is now **Skell Live**. The
underlying host automation ID remains `snapLive` so previous FL Studio
sessions and presets still load and still control the switch.

Skell Live is a **distinct low-latency pitch-controller profile**, not
a second synthesis engine:

* **Fast note acquisition:** no per-note confirmation delay in Skell Live;
  normal tuning retains the independent `noteConfirm` automation setting.
* **Motion-aware note lock:** on a steady note it holds the selected target;
  on an actively moving voice, the boundary opens and the incoming pitch
  is projected forward by at most 0.16 semitones for a responsive run.
* **Octave glitch protection:** one isolated detector octave error cannot
  change the target. A real octave sung for two consecutive detector hops
  is confirmed without a long wait (typically about 2 ms extra).
* **Breathy rap/melodic tracking:** a slightly lower voiced-confidence floor
  acquires legitimate borderline-vocal estimates earlier. The detector's
  own voicing and octave guards remain active.
* **Negative Speed:** the original instant snap and overload animations
  still work. Skell Live's octave and confidence handling remain active
  even at -3 ms, where ordinary hysteresis shrinks to zero.

**Note Stabilizer has been removed from the visible plugin**: the toggle,
None/Short/Mid/Long pills, mouse actions and reset/preset-loading logic are
gone. Old `stabilizer` and `stabilizerTime` IDs remain in APVTS as
**inactive compatibility entries**, preventing broken saved DAW sessions.
Their values no longer change the DSP. The separate advanced
`noteConfirm` automation still works in normal mode, but Skell Live always
prioritizes immediate note acquisition.

Skell Live requires **Low Latency enabled** and shares the same genuine
3.4 ms high-register delay; switching it doesn't change host PDC.

### Negative-retune orb animation

The current MetaTune UI and resizable canvas are preserved. Below 0 ms, **the Speed knob enters Lyra's overload state**: its indicator trembles subtly, lit ticks flicker red-white, the metal ring gains a pulsing red halo, a zero-ms detent becomes hot, and a 350ms shockwave fires on the transition into negative retune. These overlays are computed by the GUI timer and preserved by the existing MetaTune C-shaped knob housing.

Below 0 ms, the orb's **middle violet ring transitions toward hot pink**, with a smooth pink bloom, moving white-pink pitch-class glint, and an arc showing the detected-to-**actually corrected** pitch. These mirror LyraTune's negative-retune visual cues without replacing MetaTune's orb artwork. The GUI animation is driven exclusively by the editor's 60 Hz timer and follows automated Speed changes as well as mouse drags.

### Latency and register limitation

**Low Latency on** now uses the original Lyra read-head engine with a **160 Hz-safe centre delay**, maintaining the independent pitch detector's 70 Hz floor. The host is told the true delay, not a theoretical output-delay target.

| Sample rate | Ultra Live PDC | Time |
|---|---:|---:|
| 44.1 kHz | 151 samples | 3.424 ms |
| 48 kHz | 164 samples | 3.417 ms |
| 96 kHz | 325 samples | 3.385 ms |

* **170 Hz and above:** full LyraTune retune processing and negative-Speed snapping at ~3.4 ms.
* **155–170 Hz:** sample-smoothed transition toward uncorrected source pitch.
* **Below 155 Hz:** input stays audible at its natural pitch; attempting to splice a full 70 Hz cycle at only 3.5 ms of history would require future samples or cause artifacts.
* **Low Latency off (Quality):** full-range 70 Hz LyraTune reconstruction with 2 ms detector lookahead, approximately **9.5 ms at 48 kHz**. Suitable for low-voice tuning.
* **Skell Live:** adaptive, fast note-tracking controller using the same shifter and PDC as Ultra Live; not a new full-range synthesis mode.

The current **3.5 ms target is intentionally limited to higher registers**. This is a quality/physics tradeoff, not a hidden failure to retune bass notes. True high-quality 70 Hz pitch correction at 3.5 ms requires a separate validated low-register synthesis approach. Low Latency changes notify the DAW of PDC on the message thread; aligned dry, bypass and reference tone follow the selected budget. Plugin latency is not audio-device round-trip latency.

### Original source

The engine code in `Source/LyraDSP` is copied directly from the user-supplied archive `tune demo that sounds better than metatune.zip` (LyraTune-main), whose embedded source revision matches `333stack/LyraTune` commit `5f611543a21e3cf9b7ac4bc06ed9f854cde45235`. The imported DSP retains its original pitch detector, pitch-target controller and stereo-linked shift architecture. Integration changes add anti-phase-safe analysis, host-sample sanitization, a bounded Ultra Live mode, click-softened read-head changes and truthful applied-correction telemetry.

The original DSP suite is preserved in `tests/LyraDSP`. It tests negative retune, octave changes, vibrato, stereo, range switching, formant accent, CPU safety and allocation-free callbacks. Extra Skell Live tests verify genuine octave leaps versus one-frame aliases, immediate rap/melodic note acquisition, confidence handling, unchanged PDC, negative retune and removed-stabilizer backward compatibility.

### Build and tests

A JUCE 8 checkout and CMake 3.22+ are needed:

```powershell
cmake -S . -B build -DJUCE_PATH=C:/JUCE -DSKELLTUNE_BUILD_TESTS=ON
cmake --build build --config Release --target MetaTuneInspired_VST3 LyraOriginalDSPTests
ctest --test-dir build -C Release --output-on-failure
```

GitHub Actions runs the DSP suite on Linux, the VST3 build plus regression tests on Windows, and a native JUCE VST3 host smoke that scans the built plugin, checks its PDC and bypass impulse, renders the editor at compact/medium/full sizes and captures before/after screenshots of negative-retune glow. **Final sound quality and behavior in FL Studio should also be auditioned with real vocals**, especially fast note changes, low voices, negative retune and stereo/bypass transitions.

### Licensing and distribution

Read `LICENSE` and `THIRD_PARTY_NOTICES.md` before distributing binaries. Legacy MetaTune DSP has AGPL-derived provenance; the supplied LyraTune archive did not include a LICENSE file. The source revision is recorded for provenance, **not** as a claim that importing a public repository automatically grants distribution rights.
