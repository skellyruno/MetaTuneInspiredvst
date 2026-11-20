// Regression for repeated low-register LAB renderer re-entry.
// A previously used SnapLiveShifter must re-arm its wet/dry fade on EVERY
// off -> on transition, not merely on its first enable after prepare().
// Standalone C++17; exercises the exact production header with no JUCE.
#include "SnapLiveShifter.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char* message)
{
    if (!ok)
    {
        std::fprintf(stderr, "lowvoice re-entry fade FAILURE: %s\n", message);
        std::exit(1);
    }
}

static float sampleNoise(uint32_t& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const float normalized = float(state & 0xffffu) / 65535.0f;
    return 0.8f * (normalized - 0.5f);
}

int main()
{
    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
        autotune::SnapLiveShifter shifter;
        // Same nominal PDC as the isolated 2.5ms LAB VST3; not a wet
        // source-age guarantee for voiced pitch-cycle splices.
        shifter.prepare(sr, int(std::lround(sr * 0.0025)));
        shifter.setRatio(1.27f);
        shifter.setPeriod(float(sr / 103.826174)); // G#2
        uint32_t state = 0x5A17B36Du;
        int checked = 0;
        for (int cycle = 0; cycle < 12; ++cycle)
        {
            shifter.setRenderingEnabled(true);
            // Run longer than the 8ms fade so the previous fade has ended.
            for (int i = 0; i < int(sr * 0.045); ++i)
            {
                const float y = shifter.processSample(sampleNoise(state));
                require(std::isfinite(y), "rendered audio must remain finite");
            }
            shifter.setRenderingEnabled(false);
            for (int i = 0; i < int(sr * 0.006); ++i)
            {
                const float y = shifter.processSample(sampleNoise(state));
                require(std::isfinite(y), "disabled history must remain finite");
                require(std::abs(y - shifter.getLastAlignedDry()) < 1.0e-7f,
                        "disabled renderer must produce the aligned dry tap");
            }

            // On re-entry the wet output must start very near the aligned
            // dry tap; without re-arming renderFade the old implementation
            // jumps immediately to a displaced source head here.
            shifter.setRenderingEnabled(true);
            const float first = shifter.processSample(sampleNoise(state));
            const float dry = shifter.getLastAlignedDry();
            require(std::isfinite(first) && std::isfinite(dry),
                    "re-entry samples must remain finite");
            require(std::abs(first - dry) <= 0.004f,
                    "first re-entry sample must be protected by fresh fade-in");
            ++checked;
        }
        std::printf("lowvoice re-entry fade %.0f Hz: %d repeated transitions passed\n",
                    sr, checked);
    }
    return 0;
}
