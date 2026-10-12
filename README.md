# MetaTune Inspired (prototype)

An original JUCE VST3/Standalone starter project inspired by the supplied screenshot. It is **not** the original plugin and is not affiliated with its maker.

## Current prototype scope
- JUCE plugin project for Windows/macOS/Linux builds (VST3 + Standalone target).
- Dark, purple/cyan GUI inspired by the screenshot.
- Host-automatable Speed, Amount, Sustain, scale, root-note, Note Stabilizer and Low Latency parameters.
- Basic autocorrelation pitch estimate for the readout.
- Initial `juce::dsp::PitchShifter` processing path.

## Important limitation
This is a first-pass engineering scaffold, **not a finished transparent vocal tuner**. Pitch detection and pitch shifting are separate hard problems: this version does not yet implement production-grade per-frame scale-locked correction, formant preservation, robust voiced/unvoiced handling, or the original plugin's proprietary algorithms. The speed/amount controls are scaffold parameters and need proper smoothing/correction logic and listening tests before use on important sessions.

## Build on Windows for FL Studio
1. Install Visual Studio 2022 with the **Desktop development with C++** workload.
2. Download JUCE 7.0.8 or newer and extract it, e.g. `C:\\JUCE`.
3. From a Developer PowerShell:
   ```powershell
   cmake -S . -B build -DJUCE_PATH=C:/JUCE -A x64
   cmake --build build --config Release
   ```
4. Find the VST3 under `build/MetaTuneInspired_artefacts/Release/VST3/`.
5. Copy the `.vst3` bundle to your user VST3 folder or a custom FL Studio plugin path, then rescan plugins in FL Studio.

If your JUCE version does not provide `juce::dsp::PitchShifter`, update JUCE or replace that stage with a dedicated pitch-shifting implementation.

## Next engineering milestones
1. Add buffered, windowed pitch tracking with confidence and hysteresis.
2. Implement a true target-note correction loop that changes pitch continuously rather than one block-wide estimate.
3. Add scale/key UI state, note selection, keyboard highlighting, and latency reporting.
4. Add parameter smoothing, dry/wet, stereo-linking, denormal handling, and real-time stress tests.
5. Compare against reference vocal clips and tune artifacts, latency, and response.
