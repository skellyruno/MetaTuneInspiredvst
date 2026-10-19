# SkellTune V15 — Clean source / pitch-lock repair

**Status:** portable C++ DSP development project. Includes JUCE processor/editor and Windows VST3 + Standalone build configuration. The Windows binaries have **not** been built or validated in FL Studio here.

V15 fixes the biggest measurable weakness in earlier hard-tune renders: the **time-varying post-synthesis latency compensator** was bending the pitch of otherwise corrected audio. An explicitly enabled pitch-preserving delay path now keeps its wet read tap stationary on voiced audio instead of chasing changing grain-latency estimates. The independent full-delay path for non-synthesized/generated audio is retained.

This substantially improves the A-major hard-tune comparison on the project owner's recorded vocal. See `V15_ENGINEERING_REPORT.md` for test methods, results, remaining time-alignment error, and licensing considerations.

## Contents (production-focused)

- `Source/`: JUCE processor/editor plus portable C++17 tuning and PSOLA engine.
- `tests/`: 30 DSP regression tests, including variable-delay pitch modulation, multirate stability, low-frequency tracking, stereo, NaN recovery, and real-time safety.
- `tests/assets/real_voice_octave_200hz.wav`: **80 KB private test excerpt** retained because it catches a genuine octave-tracking regression; do not distribute publicly without permission.
- `tools/render_wav.cpp`: offline WAV renderer, matching the current pitch-preserving processing path.
- `tools/compare_vocals.py`: timing-aligned real-vocal comparison (requires numpy, scipy, librosa).
- `tools/formant_benchmark.py`: synthetic or supplied-vocal spectral-envelope diagnostic (requires numpy, scipy, soundfile).
- `tools/benchmark_cpu.cpp`: portable callback-cost benchmark.
- `.github/workflows/windows-vst3.yml`: Windows JUCE/MSVC build; not verified in this environment.
- `docs/THIRD_PARTY_NOTICES.md`: licensing and attribution, including inherited AGPL-3.0 code.

Historical releases, private full-length raw/processed vocal recordings, commercial binaries, failed experimental DSP branches, research archives, build outputs, and old reports are **excluded**.

## Test portable DSP (no JUCE necessary)

```bash
cmake -S . -B build-dsp -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp --config Release --parallel
ctest --test-dir build-dsp -C Release --output-on-failure
```

## Build the JUCE plugin on Windows

With a local JUCE 8 checkout and Visual Studio C++ tools:

```powershell
cmake -S . -B build-vst3 -G "Visual Studio 17 2022" -A x64 -DBUILD_VST3=ON -DJUCE_PATH="C:/path/to/JUCE"
cmake --build build-vst3 --config Release --parallel 4
ctest --test-dir build-vst3 -C Release --output-on-failure
```

JUCE targets: **Skell Tune** (switchable Normal/Live engine) and **Skell Tune Live** (dedicated fast-only target), each with VST3 and Standalone formats. The configured latency budgets are nominal approximately 35.7 ms and 28.6 ms at 48 kHz. **The new stable-tap strategy sacrifices exact sample-level transport compensation**: actual delayed audio may vary a few milliseconds around the nominal budget as the source pitch changes. Refer to the report; do not market it as zero latency or exact PDC.

## Compare against your own vocal recordings

Convert a raw mono vocal to 16-bit PCM WAV (for the offline renderer):

```bash
ffmpeg -i raw.mp3 -ar 48000 -ac 1 -c:a pcm_s16le raw48.wav
./build-dsp/autotune_offline_renderer raw48.wav v15_live.wav --key 9 --live --precision
./build-dsp/autotune_offline_renderer raw48.wav v15_normal.wav --key 9 --normal --precision
python tools/compare_vocals.py --raw raw.mp3 --reference reference.mp3 --render v15_live.wav --name 'V15 Live'
```

`--key 9` is A major. The user-supplied reference recording and any private source files are **not** bundled in this cleaned package.

## Licensing

Parts of the engine are marked as derived from **MicFX (AGPL-3.0)**. Retain the original source attribution and review your redistribution, corresponding-source, and license-delivery obligations before publishing or selling the plugin. See `docs/THIRD_PARTY_NOTICES.md`. No proprietary MetaTune code or installer is included.
