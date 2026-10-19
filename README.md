> **October 18 update:** Start with [ADVANCED_DSP_PASS_2026-10-18.md](ADVANCED_DSP_PASS_2026-10-18.md) for the latest confidence-aware tracking, consonant protection, CPU/latency measurements, and the formant benchmark. This remains source code, not a Windows VST3 build.

# MetaTuneInspired — DSP reliability / PDC update (2026-10-16)

This is a **source update**, not a verified VST3 release. It continues the uploaded `MetaTuneInspired_DSP_LowLatency_Pass` source plus the later `RealtimeAutotune.h` grain-support improvement. Original JUCE parameter IDs, existing DSP pitch scheduler and original standalone test suites are preserved.

## Install into the EXISTING JUCE project

Replace these three files under your current plugin's `Source/` directory, leaving the original GUI/PluginEditor, CMake/JUCE configuration, and resources in place:

- `Source/RealtimeAutotune.h`
- `Source/PluginProcessor.cpp`
- `Source/PluginProcessor.h`

Use the `tests/` directory and **this separate CMakeLists.txt only for the standalone C++17 DSP tests**. Do not replace your JUCE project's CMakeLists.txt with this test-only one.

## Changes made and why

1. **No telemetry loss on oversized host blocks:** the previous processor silently dropped intrinsic-delay telemetry when a host block exceeded the preallocated per-channel capacity, causing incorrect host latency alignment. The processor now runs the full signal chain in sequential bounded slices (up to min(expectedBlock, 4096) frames), keeping channel-major delay telemetry and fixed compensation intact. Scratch storage is preallocated in `prepareToPlay()`, never resized in `processBlock()`.
2. **Block-invariant stereo analysis:** changed stronger-channel selection from once-per-block energy scanning to per-sample 40 ms exponentially smoothed energy with the same 1.45x switching hysteresis. This removes dependence on host block size and is tested against whole-block processing, single-sample blocks and 16,384-frame blocks.
3. **Correct reference-piano host compensation:** the reference tone is created locally with no vocal pitch-shifter delay. Previously, it was mixed into the tuned signal and subsequently padded using the *vocal* intrinsic-latency telemetry. It now uses an independently preallocated `FixedDirectDelay` of exactly the host-reported duration and is mixed **after** vocal compensation. Preview is faded off on global bypass.
4. **No scheduler accuracy regression:** the previous ratio-aware short-grain/low-latency source scheduler is unchanged. Source selection experiments with a larger phase bias failed tuning and were rejected.
5. **More coverage:** added `RealtimeAutotune_block_regression.cpp`, and `RealtimeAutotune_quality_grid.cpp` for synthetic sine measurements across 44.1/48/96 kHz, 10 fundamentals from ~77.8 to 466 Hz, and both analysis modes. Supplied test-only CMake/CTest configuration.

## Standalone validation done in this environment

With GCC and `-std=c++17 -O2 -Wall -Wextra -Wpedantic`, all three test suites passed:

- `RealtimeAutotune_validation.cpp`: original 14 top-level checks.
- `RealtimeAutotune_latency_regression.cpp`: corrected-note latency, output cents, RMS, note transitions, vibrato.
- `RealtimeAutotune_block_regression.cpp`: stereo audio + per-channel telemetry invariant to block sizes 1/17/63/127/512/4096/16384 against a 26,001-sample block; Amount=0 and bypass modes; direct and independently generated tone PDC impulses.

All **three** passing suites were also run with GCC `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`, with leak detection and sanitizer halt-on-error enabled; all passed.

The quality grid measured 60 synthetic steady-tone cases (10 notes × 3 sample rates × 2 modes): **0 outliers** under its diagnostic thresholds of ±25 cents, RMS ratio 0.75–1.25, and 95th-percentile delay under 36 ms. See `docs/quality_grid.csv`. This is not a real vocal quality test, a measurement of pitch shifts during consonants, or a hard guarantee of lookahead on all signals. The lower fundamentals remain far from the sub-2.5 ms intrinsic target.

Representative 48 kHz Low Latency intrinsic delay medians (sine inputs):

| Input | Target MIDI | Median | p95 |
|---|---:|---:|---:|
| 77.78 Hz | 40 (E2) | ~14.35 ms | ~19.4 ms |
| 116.54 Hz | 47 (B2) | ~9.1 ms | ~13.4 ms |
| 233.08 Hz | 59 (B3) | ~4.59 ms | ~6.74 ms |
| 275 Hz | 60 (C4) | ~4.18 ms | ~5.84 ms |
| 300 Hz | 62 (D4) | ~3.82 ms | ~5.30 ms |

The *reported and compensated host latency stays at about 35.7 ms*, by design. Sub-2.5 ms **corrected-low-fundamental** latency is not achieved, and even a hypothetical intrinsic reduction would not reduce the end-to-end delay unless the safe fixed host budget and scheduler were also redesigned and revalidated.

## Build portable regression tests (Windows or Linux)

With a C++17 compiler and CMake available, from **this test package directory**:

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests --config Release
ctest --test-dir build-tests --build-config Release --output-on-failure
```

To rerun the diagnostic CSV (no JUCE needed):

```sh
./build-tests/autotune_quality_grid > quality_grid.csv
```

On Windows/MSVC, the executable is typically under `build-tests/Release/` rather than `build-tests/`.

## Not yet verified / known limitations

- The supplied material lacks the original `PluginEditor.h/.cpp`, plugin resources and complete VST3 project/CMake config, so the **real Windows MSVC/JUCE VST3 target was not compiled**. Existing editor integration and plugin-load behavior cannot be claimed tested.
- **FL Studio playback, host PDC against actual parallel tracks, listening tests and multi-instance CPU use** were not run here.
- Changing real-time code in the processor requires a full Windows build and actual DAW integration checks. Particularly verify reference-piano timing, global bypass fade, mono/stereo transitions, and unexpectedly large blocks with real vocal audio.
- A single scalar intrinsic delay estimate for a mixed dry/wet path is approximate during fades and pitch transitions; the compensator's 4 ms crossfade means transient alignment can still have short smears. The fixed latency budget is validated on a synthetic grid, not mathematically established for every possible input.
- The header is marked derived from MicFX and AGPL-3.0. Preserve attribution, investigate corresponding-source/distribution obligations and include the actual license before distributing a combined VST3. See `docs/THIRD_PARTY_NOTICES.md`.

## October 17, 2026 verified DSP iteration

This pass deliberately does **not** claim sub-2.5 ms intrinsic correction or a full VST3 build.

- Reduced the Low Latency PSOLA correlation-template half-width from 0.10 to 0.09 input periods; retained the previous proven source scheduling and 0.05-period search reach. More aggressive 0.07/0.08 experiments failed a stricter pitch gate on the 275 Hz to C4 correction (~+10 cents) and were rejected.
- Converted the synthetic 60-case three-sample-rate pitch quality grid into a **real CTest assertion**. Previously it returned zero regardless of detected outliers. CTest now includes four regressions rather than three.
- The targeted regression now enforces a tighter 5-cent tolerance on the 275 Hz to C4 test, in addition to the prior general 25-cent bound.
- At 48 kHz, Low Latency median / p95 intrinsic delay (ms): 233.08 Hz → B3 **4.55 / 6.70** (previously 4.59 / 6.74); 275 Hz → C4 **4.14 / 5.80** (previously 4.18 / 5.84); 300 Hz → D4 **3.73 / 5.26** (previously 3.77 / 5.29). Steady correction errors were -1.08, -2.65, and -1.07 cents, respectively.
- No host-reported latency was reduced; approximately 35.7 ms fixed PDC budget remains. Real corrected-pitch sub-2.5 ms and Windows MSVC/JUCE/FL Studio validation remain open work.

Reproduce with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure`. The CMake target is standalone DSP testing only.
