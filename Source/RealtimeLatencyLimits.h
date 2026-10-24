#pragma once

// Single-source limits for the production monitoring signal path.
// Values are DSP source-age budgets, not round-trip audio-device latency or
// guaranteed response time of the independent pitch estimator.
namespace autotune::latency_limits
{
constexpr double liveMs = 3.5;
constexpr double qualityMs = 8.0;
constexpr double doublerCentreMs = 1.0;
constexpr double doublerDepthBaseMs = 0.25;
constexpr double doublerDepthWidthMs = 0.50;
constexpr double totalWorstCaseMs = qualityMs + doublerCentreMs
                                + doublerDepthBaseMs + doublerDepthWidthMs;
static_assert(totalWorstCaseMs <= 9.8,
    "Production DSP paths must retain room below the hard 10ms history budget");
} // namespace autotune::latency_limits
