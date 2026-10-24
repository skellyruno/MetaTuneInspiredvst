# Skelly MetaTune — V1.11 Minimal Source

This is the **trimmed development/build baseline** for the V1.11 JUCE VST3 and Standalone plugin. DSP, parameters, IDs and GUI source are unchanged from V1.11. It is not a compiled Windows plugin.

## Contents

- `Source/`: complete live plugin DSP, processor and editor; includes both DSP headers required by `RealtimeAutotune.h`.
- `CMakeLists.txt`: JUCE 8.0.8 / C++17 VST3 + Standalone build, with optional portable tests and VST3 host smoke executable.
- `.github/workflows/build-windows.yml` + `tools/windows_vst3_smoke.cpp`: Windows binary build, runtime scan/instantiate and state/processing checks in CI.
- `tests/`: 18 focused portable regressions covering the current pitch tracking, vibrato, octave handling, low-register carrier/formants, stereo, bounded Live latency and realtime safety.
- `LICENSE` and `THIRD_PARTY_NOTICES.md`: retain upstream source and licensing obligations.

Removed: historical release notes, prior-version CSV evidence, offline ML training/tools, obsolete benchmark scripts, Python tests depending on removed research utilities, and vocal WAV fixture of uncertain publication rights. No code was removed from `Source/`. The larger V1.11 archive remains the historical/full-test baseline.

## Windows build

Install JUCE 8.0.8 and Visual Studio C++ tools, then run (PowerShell):

```powershell
cmake -S . -B build -DJUCE_PATH=C:/JUCE -A x64 -DSKELLTUNE_BUILD_TESTS=ON -DSKELLTUNE_BUILD_WINDOWS_HOST_SMOKE=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build --build-config Release --output-on-failure
```

The GitHub Actions workflow runs the build and host smoke test. **It has not been executed as part of this local cleanup.** JUCE is not bundled.

## Portable DSP tests (no JUCE needed)

```bash
cmake -S tests -B build-dsp -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp --parallel 4
ctest --test-dir build-dsp --output-on-failure
```

## Timing caveat

Live is designed for a 3.5 ms *transport/history budget* and Quality for 8 ms. These figures are **not** proofs of a fixed 3.5 ms corrected-wet waveform delay, note detection time or microphone-to-headphone round-trip latency. Native Windows VST3 and FL Studio stability remain to be verified.

## Licensing

This derivative includes code credited to MicFX and is distributed with the included AGPL-3.0 and third-party notices. Review JUCE and upstream licensing obligations before distribution of binaries.
