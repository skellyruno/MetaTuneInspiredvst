# MetaTune Inspired

An original pitch-correction VST3 starter project inspired by the supplied GUI reference. It is **not** the original MetaTune plugin and is not affiliated with or endorsed by its maker.

## Windows 10 + FL Studio build workspace

### Recommended: build it on GitHub

This repository includes a GitHub Actions workflow at `.github/workflows/build-windows.yml`. It checks out JUCE, configures a Windows x64 build, builds the VST3, and publishes the result as a downloadable workflow artifact.

1. Create a new **empty GitHub repository**.
2. Extract this project ZIP locally.
3. Open the extracted folder in GitHub Desktop, or initialize and push it from a terminal:
   ```powershell
   git init
   git add .
   git commit -m "Initial MetaTune Inspired workspace"
   git branch -M main
   git remote add origin https://github.com/YOUR-USERNAME/YOUR-REPOSITORY.git
   git push -u origin main
   ```
4. On GitHub, open **Actions**. If prompted, enable workflows.
5. Open **Windows VST3 build**, choose the latest run, and wait for all steps to pass.
6. Download the `MetaTuneInspired-Windows-VST3` artifact from the completed run. GitHub downloads artifacts as a ZIP.
7. Extract the artifact. Copy `MetaTune Inspired.vst3` (the VST3 bundle/folder) to `C:\Program Files\Common Files\VST3`.
8. Restart FL Studio, open **Options → Manage plugins**, and run a scan. Search for `MetaTune Inspired`.

GitHub Actions artifacts are available only when the workflow succeeds. If the build fails, open the failed step's log and share it so the compile issue can be fixed.

### Build locally on Windows 10

Install Visual Studio 2022 with **Desktop development with C++**, CMake 3.22 or newer, and Git. Then from a Developer PowerShell in this project folder:

```powershell
git clone --branch 8.0.8 --depth 1 https://github.com/juce-framework/JUCE.git JUCE
cmake -S . -B build -DJUCE_PATH="$PWD/JUCE" -A x64
cmake --build build --config Release --parallel
```

The VST3 output should be under `build\MetaTuneInspired_artefacts\Release\VST3\`.

## Prototype scope

- JUCE C++ project configured for VST3 and Standalone formats.
- Dark purple/cyan GUI with Speed, Amount, Sustain, scale, root-note, Note Stabilizer, and Low Latency controls.
- Basic autocorrelation pitch estimate for the UI readout.
- Initial JUCE pitch-shifter processing stage.
- GitHub Actions workflow to compile and publish a Windows x64 VST3 artifact.

## Audio-quality disclaimer

This is a **development prototype, not yet a production-quality automatic vocal tuner**. The current pitch estimate is a lightweight autocorrelation method and the processing is not a complete frame-by-frame scale-locking correction algorithm. It does not yet provide reliable formant preservation, robust voiced/unvoiced detection, or the original plugin's proprietary behavior. Test on copies of audio and at conservative levels. Further engineering and listening tests are needed before using it in important sessions.

## Next milestones

1. Implement smooth, frame-based target-note correction.
2. Improve pitch tracking confidence, octave error handling, and voiced/unvoiced detection.
3. Add explicit note selection and keyboard highlighting.
4. Improve latency reporting, parameter smoothing, stereo behavior, and preset handling.
5. Test against a range of voices and instruments, then tune artifacts and response.
