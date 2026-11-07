# Third-party notices and licensing

This project includes DSP code identified in `Source/RealtimeAutotune.h` as derived from **MicFX (AGPL-3.0)**. Preserve the upstream attributions and comply with the corresponding obligations when distributing modified source and compiled plugins.

The V18 two-period voiced-onset validation was developed by studying the **NoteForge** source code supplied by the user, particularly `Source/dsp/LiveTuner.cpp` (its two-period pitch-validation logic) and `Source/dsp/PitchDetector.cpp`. NoteForge is licensed **GNU Affero General Public License v3.0** according to its README and included LICENSE. The new implementation uses the existing SkellTune YIN CMNDF data path, not NoteForge's complete engine, UI, audio samples, or FL bridge.

A copy of the **AGPL-3.0 license** is included as `LICENSE`. This is an engineering source package, not a legal determination of license compatibility. Review upstream notices and distribute required corresponding source when shipping derivatives. JUCE itself has separate licensing terms to review for your distribution.

The inherited private vocal fixture has been excluded from this minimal archive because public distribution rights were not established.

V19 added an original bounded cycle-synchronous read-head wrap inspired by comparing NoteForge live synthesis; no upstream NoteForge source files are included. V20 builds on that algorithm by adding a target-trajectory stability gate.

V29 additionally audited the user-supplied **HelixTune** (MIT), **SnapTune** (AGPL-3.0), and **PitchZazz** (MIT declared in `Cargo.toml`) reference projects. Their pitch-continuity, live resampling, and asynchronous-block design ideas informed architecture decisions, but no new source files from those repositories were transplanted into V29. The stereo handoff code is an independent implementation based on standard signal correlation and energy tracking.

V1.13 ("Snap Live") adds `Source/SnapLiveShifter.h`, a **derived port of the Live renderer of SnapTune** (`Source/dsp/AutoTuneEngine.cpp`: the delay-line read tap, the cross-correlation splice search `findSpliceLength`, and the splice/crossfade scheduling in `maybeSplice`/`liveSample`). It is re-implemented against Skelly's own detector and correction controller and changes the algorithm in several ways (see its header comment and README), but it is derived work: keep SnapTune's attribution and licence with it. SnapTune is recorded above as AGPL-3.0, the same licence as this project. The `snaptune-main` archive supplied for the port contained no LICENSE file, so confirm the upstream licence text before distributing binaries. SnapTune's telemetry, update-check, UI, presets and bundled fonts (Literata, Rokkitt; both OFL) were **not** ported.
