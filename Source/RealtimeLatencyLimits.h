#pragma once

// Single-source limits for the production monitoring signal path.
// Values are DSP source-age budgets, not round-trip audio-device latency or
// guaranteed response time of the independent pitch estimator.
namespace autotune::latency_limits
{
constexpr double liveMs = 3.5;
constexpr double qualityMs = 8.0;
// Snap Live (SnapTune-derived pitch-synchronous renderer): nominal tap delay, reported to the host.
// Unlike Live/Quality this is a *nominal* figure: corrected voiced audio wanders about it inside
// the splice window of +-~0.6 source period (see SnapLiveShifter.h).
constexpr double snapLiveMs = 4.0;
constexpr double doublerCentreMs = 1.0;
constexpr double doublerDepthBaseMs = 0.25;
constexpr double doublerDepthWidthMs = 0.50;
constexpr double totalWorstCaseMs = qualityMs + doublerCentreMs
                                + doublerDepthBaseMs + doublerDepthWidthMs;
static_assert(snapLiveMs <= qualityMs && snapLiveMs > liveMs,
    "Snap Live nominal latency must sit between Live and Quality");
static_assert(totalWorstCaseMs <= 9.8,
    "Production DSP paths must retain room below the hard 10ms history budget");
} // namespace autotune::latency_limits
