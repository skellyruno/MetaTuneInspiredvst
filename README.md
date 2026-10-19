# MetaTuneInspired — V15 engine integration

This is the **original uploaded MetaTuneInspiredvst-main** project with its original JUCE GUI and **unchanged** `.github/workflows/build-windows.yml`. The plugin target name, bundle ID, and workflow output location are unchanged.

## DSP changes

Integrated the SkellTune V15 pitch-preserving delay compensator, precision hard-tune engine, latency-aligned global bypass, audio-thread parameter snapshotting and safeguards for non-finite float audio/telemetry. Updated the original processor to match that engine; added V15 regression tests and offline diagnostics. Original editor `Source/PluginEditor.cpp/.h` is retained byte-for-byte.

**Preset compatibility:** existing parameter identifiers were preserved; extra `precisionHardTune` and `customDegree0` through `customDegree11` parameters were added. The original UI doesn't expose the new precision control, which defaults off. Doubler mix now defaults to zero for new instances, but existing saved settings should restore their values.

## GitHub Actions

No edits to `.github/workflows/build-windows.yml`. It still checks out JUCE 8.0.8, configures with `JUCE_PATH`, builds `MetaTuneInspired`, and uploads the Release VST3 artifact from the original path. **The Windows JUCE plugin build has not been run here.**

## Portable validation (no JUCE required)

```sh
cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests --config Release -j
ctest --test-dir build-tests --build-config Release --output-on-failure
```

All 30 portable DSP tests passed under GCC on Linux after providing the vocal test asset path via CTest. Use `-DSKELLTUNE_BUILD_TESTS=ON` to add them to the full JUCE project build (optional, default OFF).

## Windows plugin build

Run the existing GitHub Actions **Windows VST3 build** workflow, or locally:

```powershell
cmake -S . -B build -DJUCE_PATH=C:/JUCE -A x64
cmake --build build --config Release --parallel
```

This still needs Windows MSVC/JUCE compilation and an FL Studio smoke test. Build problems specific to JUCE 8's UI API may remain in the unmodified original editor; the engine regressions do not compile JUCE classes. See `V15_ENGINEERING_REPORT.md` for pitch accuracy and latency limitations. The pitch-preserving fixed compensation sacrifices exact dynamic transport alignment.

## Licensing and privacy

DSP includes AGPL-3.0-derived MicFX code. Review licensing obligations before distribution. `tests/assets/real_voice_octave_200hz.wav` is a short private vocal excerpt used for regression and should not be publicly redistributed without permission.
