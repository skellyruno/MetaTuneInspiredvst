# SkellTune V15 — pitch-preserving latency compensation

Date: 2026-10-19. Development update; **not a validated commercial VST3 release**.

## Root cause

The prior post-synthesis compensation stage chased `PsolaShifter::getLatencySamples()` **on every audio sample**. The compensator's continuously moving read tap effectively performed an additional variable-rate resampling of the tuned audio. Even when the synthesis pulse clock correctly targeted an A-major note, the downstream delay modulation could pull its audible fundamental back toward the singer's original pitch. Earlier tests measured the clock or the shifter separately, failing to catch what the host-facing, processed WAV actually contained.

V15 adds `OutputLatencyCompensator::setPitchPreserving(true)`:

- For processed audio with nonzero intrinsic-latency telemetry, hold a constant wet padding chosen at prepare time (`round(0.60 * nominalHostDelay)`), rather than moving its read tap to follow every source-grain delay change.
- For direct, generated, or no-telemetry audio, retain the exact full host delay.
- The JUCE processor enables pitch-preserving compensation in the active effect path, and the offline renderer uses the same mode. Global Bypass can return the wet compensator to its legacy tracked behavior to preserve the previous bypass transition path.
- No extra heap allocations, processing lookahead, new parameter IDs, or new core pitch-shifter modes.

The wet padding fraction is a **fixed engineering approximation**, not a proof of exact, note-invariant transport latency. See limitations below.

## Actual owner-provided vocal comparison

Input: supplied ~28.7-s raw vocal. Reference: supplied Auto-Tune Pro A-major render. Same 48 kHz PCM source, A major, Precision Hard Tune, Live mode. The script `tools/compare_vocals.py` automatically estimates relative timing from the amplitude envelope (not from the pitch error), uses `librosa.yin` on selected reasonably tonal/high-energy frames (n=1,576), and measures median/p90 absolute differences. Lower is better.

| Selected-frame pitch metric | V14 (moving pad) | V15 (stationary pad) |
|---|---:|---:|
| Median difference from Auto-Tune Pro | 24.78 cents | **3.18 cents** |
| 90th-percentile difference from Auto-Tune Pro | 81.64 cents | **38.33 cents** |
| Median distance from nearest A-major note | 26.23 cents | **3.09 cents** |

Raw vocal's median distance from A major was ~31.18 cents, and the Auto-Tune Pro reference's own median A-major distance was ~1.60 cents. Thus the direct-reference median improved **87.2%**, and the A-major pitch-centering median improved **88.2%** versus V14. The latter is the largest demonstrated gain since the first voice benchmark. These are selected-frame F0 measurements, not a listening study or proof of equal sound quality.

V15 Normal Precision: **3.02 cents** direct-reference median and **83.39 cents** p90. Live performed better on upper-tail errors and remains preferable for fast hard-tune on this recording. Results may vary with other voices, consonants, and note ranges.

## Formant / level-matched vowel

With the unchanged synthetic source/filter test vowel, the existing `tools/formant_benchmark.py` reports a level-matched smoothed spectral-envelope shape error of **5.57 dB for V14** versus **4.99 dB for V15**. Measured output/dry level was approximately -0.02 dB vs +0.00 dB respectively. This is a controlled synthetic diagnostic; it is not a substitute for real-vocal formant and timbre listening tests.

## Regression suite

The cleaned production tree retains **30 CTest DSP tests**: the legacy source-reference controller smoke test was deliberately removed with its large research-only dependency. Two new tests protect the cause of the real-vocal improvement:

1. `autotune_pitch_preserving_pdc`: 48-kHz tone with adversarial sinusoidal intrinsic delay telemetry, block-boundary invariance, and sample-accurate direct-impulse padding. On this synthetic case the old moving tap gives about **85 cents p95 short-window pitch error**, while the stable path gives about **0.0001 cents**.
2. `autotune_stable_pdc_multirate`: independently checks pitch-preserving reconstruction at 44.1, 48 and 96 kHz for 220/440/660-Hz tones and compares against the old moving-delay mode.

These are specific anti-regression tests, **not** proof that every real-vocal phrase is perfectly pitched. The old engine's core quality, octave, adversarial shift, formant-research and latency-budget tests are kept where they exercise active production code.

## Latency / honest tradeoff

The host still advertises a **nominal fixed delay** (dedicated Live: 2 periods at a 70-Hz tracking floor, ~1,372 samples/28.6 ms at 48 kHz). A moving output read tap can force its output to follow intrinsic timing changes, but changes the pitch while doing so. The pitch-preserving constant wet padding avoids that detuning, but the shifter's actual intrinsic transport delay changes with source pitch.

For this raw recording, the wet `padding + estimated intrinsic - advertised budget` distribution was approximately **-244 samples median** (-5.08 ms), **-25 samples p95** (-0.52 ms), **+115 samples p99** (+2.40 ms), and **+349 samples observed maximum** (+7.27 ms). These are estimates based on the engine's internal latency telemetry, not acoustic impulse/host measurements. **This mode is not sample-accurate PDC** and bypass transitions can still need phase-alignment work. A different voice and pitch range may yield different extremes. Do not claim a guaranteed 28.6-ms end-to-end perceptual delay.

The next serious engineering work should address *constant-time grain scheduling* (or pulse-safe delay-slip compensation) to retain both pitch centering and tighter DAW alignment, plus consonant preservation and real-vocal formants. Re-enabling continuously moving delay taps is not acceptable as a pitch-quality fix.

## Deliberately excluded

- Failed experiments that sped up/slow down intragrain phase or magnified the requested correction ratio; they did not improve the reference-vocal benchmark.
- Historical full-length private WAVs/MP3s, diagnostic CSV archives, previous engine headers, experimental LPC research and third-party repositories, prebuilt binaries, generated build folders.
- The supplied commercial MetaTune binary and installer are not included.

The **80-KB** real-vocal octave regression fixture is retained in `tests/assets/` because it demonstrably protects a production detector bug. Treat it as private audio and remove/replace it before public distribution. Read `docs/THIRD_PARTY_NOTICES.md` and satisfy AGPL-derived source obligations before publishing.
