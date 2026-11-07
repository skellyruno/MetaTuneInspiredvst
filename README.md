# Skelly Fusion Tune Engine

This repository is the destination for **SkellyTune mini** engine consolidation, while retaining the existing **MetaTune Inspired** editor, orb, piano and all existing parameter IDs and saved-state behavior. The new integration lives on `feature/skelly-fusion-tune-engine` until the full VST3 and listening benchmarks pass.

## Architecture

```text
MetaTune UI (UNCHANGED)
  -> existing JUCE parameter/state processor
    -> SkellyFusionEngine (new shared DSP boundary)
      |-> established MetaTune YIN tracker and note-lock / onset control
      |-> independent fixed-memory NSDF periodicity verifier (new)
      |     -> conservative additional note confirmation ONLY on weak evidence
      |-> existing linked-stereo 3.5 ms Live / 4 ms Snap Live / 8 ms Quality engines
      |-> existing output latency compensation and aligned dry bypass
```

No second independent pitch-shifter is mixed into the audio output. Duplicating TD-PSOLA, Rubber Band, LPC and variable-delay synthesis inside a single callback would create competing clocks, misaligned dry signals and an unstable CPU/latency budget.

## What was integrated

* **MetaTune Inspired (primary)**: preserves its existing 3.5 ms Live, 4 ms Snap Live and 8 ms Quality renderers, YIN analysis, two-period onset checks, formant guard, phase coherence, voice-range policy, stereo linking and matched-bypass logic.
* **SkellyTune / skell-autotune-vst**: the existing MetaTune transition logic is retained rather than replacing it with a second state machine; the fusion layer adds independent evidence before extending a doubtful note-confirmation hold.
* **NoteForge**: the existing MetaTune two-period onset validation remains in place; its approach to causal low-latency correction is already reflected in the Snap Live path. Do not copy its separate high-latency editor/renderer into live recording mode.
* **Autotuner**: a new compact NSDF-style verification calculation is implemented independently in `Source/FusionPeriodGuard.h`. It does not copy the original recursive period-vote routine or any callback allocations.
* **QPitch**: kept as a listening/quality reference. Its Rubber Band renderer is not copied into Live, because its latency would prevent the 3.5–8 ms objective and its GPL obligations would need separate review.
* **AutoTuneRack**: its LPC idea remains a research candidate; the current stateful stereo shifter and unaligned dry/wet implementation are not imported.

The implementation is intentionally **selective** rather than a literal full-source merge. The alternative renderers are not claimed to have beaten the existing engine without objective and listening comparisons.

## Fusion risk controller

The new `FusionPeriodGuard` is a preallocated stereo periodicity witness. It analyzes an independent copy of the original audio (before in-place tuning), cross-checks one and two pitch periods and watches the YIN detector's two-cycle aperiodicity and confidence.

When repeated evidence is weak, it can extend *nonzero* note-transition confirmation by at most 10 ms. It does **not** add audio delay, alter host PDC, change scale, modify the pitch ratio, or affect zero-ms retune / Precision Hard Tune. All existing UI controls are unchanged. This behavior is deliberately conservative and requires real-vocal A/B tests.

## Tests

`tests/RealtimeAutotune_fusion_guard.cpp` tests periodicity at 44.1/48/96 kHz, 32–1024-sample callback chunking, anti-phase stereo, octave mismatch, and safeguards for max retune and zero confirmation.

Standalone test (does not need JUCE):

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror -ISource tests/RealtimeAutotune_fusion_guard.cpp -o fusion_guard_test
./fusion_guard_test
```

JUCE VST3 / full tests:

```sh
cmake -S . -B build -DJUCE_PATH=/path/to/JUCE -DSKELLTUNE_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

**Validation status:** the standalone guard tests pass locally. The full Windows JUCE VST3 build, DAW host loading and real-vocal artifact/latency benchmarks are still required before merging this branch into `main`. Source code comments and nominal latency budgets are not substitutes for measured performance.

## Release, license, and provenance

The source includes AGPL-derived code identified in `Source/RealtimeAutotune.h`. This branch restores `LICENSE` and `THIRD_PARTY_NOTICES.md` from repository history; do not erase copyright notices. Consult the notices, JUCE licensing and all upstream source provenance before distributing binaries or selling a proprietary plugin.
