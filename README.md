# Skelly MetaTune — V1.13 Snap Live (on the V1.12 Causal Pitch-Synchronous Live Source)

This is the **minimal source baseline** for a JUCE/C++ VST3 + Standalone pitch-correction plugin. It retains the V1.11 plugin IDs, GUI and parameter mapping, with a new Live renderer option enabled internally in 3.5 ms production Live mode. This ZIP **does not** include compiled Windows binaries.

## V1.13 changes — Snap Live

SnapTune's Live renderer is now a selectable third mode, **Snap Live** (new "Snap Live" switch, enabled only while Low Latency is on; automation ID `snapLive`, default **off**, so old sessions load unchanged). With it off, the engine's output is bit-identical to V1.12 (checked on Live and Quality, mono and stereo).

How it works (`Source/SnapLiveShifter.h`): the input is read through a fractional-delay tap that advances at the correction ratio. When the delay reaches the edge of its window the tap jumps one whole source period back or forward, the length found by normalised cross-correlation, and the two read points are crossfaded. Unlike V1.12 Live this works at **every** register, including the 85-150 Hz notes where a 3.5 ms ring cannot hold a cycle and V1.12 falls back to harmonic synthesis. Skelly's own detector and correction controller drive it; SnapTune's detector is not used.

Improvements over SnapTune's version:

- **Centred delay window** around the reported latency (nominal +-~0.6 period) instead of running from ~0 to 1.25 periods.
- **Match-gated crossfades**: coherent raised-cosine fade only for a good match (score >= 0.80); equal-power fades otherwise, so a bad splice can't produce a level dip.
- **Coarse-to-fine splice search** on a 50 Hz-2 kHz band-limited copy. Splice start cost on a 90-110 Hz note measured roughly 50-100 us versus roughly 500-1100 us at full rate (single noisy sandbox runs).
- **Linked stereo**: the pitch-analysis channel decides every splice and the other channel replays it, so channels never jump by different lengths.
- **Unvoiced re-centring waits 15 ms**, so a short detector dropout inside a vowel doesn't trigger an uncorrelated splice.
- Equal-power renderer switch (about 4 ms) between Live and Snap Live; history is kept warm so switching is click-free.

**Latency, honestly:** Snap Live reports a **nominal 4.0 ms** to the host (`latency_limits::snapLiveMs`). That is the tap delay for unvoiced audio and the dry tap. Corrected voiced audio wanders inside the splice window, roughly 0.2 ms to nominal + 0.6 period (high voices) or up to ~1.25 periods (measured up to ~14 ms at 90 Hz). It is not bounded by the 3.5 ms Live figure and is not a fixed delay. Use V1.12 Live if a hard 3.5 ms ceiling matters more than full-cycle quality on low voices.

**Offline A/B tool (V1.13):** `tools/offline_render.cpp` runs a WAV (PCM16/24 or float32, mono/stereo) through the engine in `--mode live|snap|quality` without JUCE or a DAW. Build: `g++ -std=c++17 -O2 -ISource tools/offline_render.cpp -o offline_render`, then `offline_render in.wav out.wav --mode snap`. `offline_render --synth test.wav` writes a flat, vibrato-laden test vowel line. Output is not latency-compensated; the tool prints each mode's nominal latency.

Not verified: the JUCE plugin layer (processor, editor, new toggle position at x=790,y=686 in the 1366x1080 canvas) was edited but **not compiled or looked at** here because JUCE was unavailable. The DSP was built and tested standalone: 21/21 portable tests pass (new `RealtimeAutotune_v113_snap_live`, also clean under AddressSanitizer/UBSan; a mutation check confirmed it fails when linking, search, or the window is broken). All results are synthetic tones, not real singers.

## V1.12 changes

- **Pitch-synchronous Live resampling:** a causal fractional-delay read head advances at the desired pitch-correction ratio. Where the source period and crossfade margins physically fit the same 3.5 ms ring, the head jumps by one measured source period, with ±2-sample zero-mean cross-correlation refinement and a normalized correlation acceptance threshold. Period-jump read taps are raised-cosine crossfaded to prevent clicks. Unstable periods, sudden ratio changes or broadband noise veto wrapping and fall back to the bounded history / phase-coherent synthesis path.
- **Low-register fallback:** a 3.5 ms buffer cannot contain one 85–110 Hz vocal period. Those notes keep V1.11's phase-coherent harmonic carrier and causal formant compensation rather than reading earlier source samples or silently increasing lookahead. Full-period resampling is **not** available at every pitch. Even in the applicable register, a click-free crossfade needs extra phase-matching margin, so the age of every wet sample is bounded by the 3.5 ms buffer but is not necessarily less than one source period.
- **Shorter established-note analysis:** the pitch detector retains the full-range cold-start search and octave guards, but uses a 1.05-cycle instead of 1.15-cycle YIN comparison window for an established, confident Live pitch (subject to the existing 6 ms minimum). This does not change sample buffering or automatically advance an unconfirmed note.
- **Bounded causal pitch lead:** a confidence- and trajectory-gated 3 ms extrapolation of smooth vibrato motion, capped at ±0.06 semitones (six cents). It affects only correction ratio, **not note selection or source-period estimation**. On silence, new targets, octave jumps or inconsistent slope, the lead is reset. Original V1.11 results can be A/B compared with `Settings::pitchSynchronousLive = false`.
- **Real-time safeguards:** no audio-thread allocation, no future reads, no new audio lookahead; the original 3.5 ms Live and 8 ms Quality transport budgets remain unchanged. Quality mode is untouched.

## Contents

- `Source/`: active DSP, JUCE processor/editor, licensing headers and latency limits.
- `CMakeLists.txt`: VST3/Standalone JUCE CMake build (JUCE not included).
- `tests/`: 21 portable C++ DSP regressions, including new cycle-wrap and causal vibrato checks.
- `.github/workflows/build-windows.yml` and `tools/windows_vst3_smoke.cpp`: native Windows CI build and host smoke-test recipe.
- `LICENSE`, `THIRD_PARTY_NOTICES.md`: licensing and attribution obligations.

## Windows build (PowerShell)

Install JUCE 8.0.8 and Visual Studio C++ tools:

```powershell
cmake -S . -B build -DJUCE_PATH=C:/JUCE -A x64 -DSKELLTUNE_BUILD_TESTS=ON -DSKELLTUNE_BUILD_WINDOWS_HOST_SMOKE=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build --build-config Release --output-on-failure
```

The CI workflow has not been executed for V1.12. In particular, FL Studio stability and native Windows VST3 loading have **not** been verified.

## Portable DSP validation

```bash
cmake -S tests -B build-dsp -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dsp --parallel 4
ctest --test-dir build-dsp --output-on-failure
```

In the development Linux runner the full **20/20** C++ suite passed. The two new tests passed with AddressSanitizer and UndefinedBehaviorSanitizer. New 48 kHz synthetic 220 Hz tracking probes measured average correction error of 0.04302 -> 0.02517 semitones for 4 Hz vibrato and 0.06717 -> 0.04121 for 6 Hz; there were no wrong target-note frames in those steady synthetic cases. These are not real-singer quality scores. A single local isolated shifter CPU probe at 48 kHz gave approximately 149 ns/sample for the previous shifter and 147 ns/sample with PSR enabled; measurement noise is significant, so no CPU improvement is claimed.

## Timing and quality caveats

The **3.5 ms Live number is an upper bound on transport/sample-history age**, not the actual latency of the whole corrected waveform, pitch detector, or an audio interface's round trip. Under pitch correction, sample ages vary and cannot be described as a fixed 3.5 ms voiced output delay. The detector itself requires additional observation time and no causal low-note pitch detector can infer arbitrary new notes before enough pitch evidence arrives. Causal lead approximates smooth vibrato, not unpredictable note attacks. The low-register phase-coherent fallback may retain synthetic harmonic artifacts, and transparent commercial-level vocal quality has not been demonstrated on multiple real singers.

## Licensing

Derived material retains the included AGPL-3.0 and third-party notices. Review upstream and JUCE licensing obligations before binary distribution.
