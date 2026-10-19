// Real-time pitch stability / consonant-dropout checks, with no external audio files.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

static constexpr double kRate = 48000.0;
static constexpr double kPi = 3.14159265358979323846;

int main()
{
    autotune::RealtimeAutotune dsp;
    dsp.prepare(kRate);
    autotune::Settings s;
    s.key = 0; s.scale = autotune::Scale::Major;
    s.follow = 1.0f; s.noteStabilizerMs = 0.0f; s.retuneMs = 0.0f;
    s.noteConfirmMs = 12.0f; s.consonantReleaseMs = 8.0f;
    s.voicingThreshold = 0.85f;
    dsp.setSettings(s);
    double phase = 0.0;
    auto tone = [&] (double hz) {
        phase += 2.0 * kPi * hz / kRate;
        const float out = dsp.process(0.3f * (float)std::sin(phase));
        return std::isfinite(out);
    };

    for (int i = 0; i < 18000; ++i) if (!tone(233.08)) return 1;
    if (dsp.getTargetMidi() != 59) { std::fprintf(stderr,"did not lock to B3\n"); return 2; }

    // A brief 8ms out-of-scale interjection should not become a new target.
    int incorrect = 0;
    float largestSpuriousCorrection = 0.0f;
    for (int i = 0; i < 384; ++i)
    {
        if (!tone(311.13)) return 3;
        if (dsp.getTargetMidi() != 59) ++incorrect;
        largestSpuriousCorrection = std::max(largestSpuriousCorrection,std::abs(dsp.getCorrectionSemitones()));
    }
    if (incorrect) { std::fprintf(stderr,"8ms glitch switched for %d samples\n",incorrect); return 4; }
    if (largestSpuriousCorrection > 1.5f)
    {
        std::fprintf(stderr,"unconfirmed note pulled pitch %.2f semitones\n",largestSpuriousCorrection);
        return 11;
    }
    for (int i = 0; i < 9000; ++i) if (!tone(233.08)) return 5;

    // A real, sustained note change still must be accepted quickly.
    int acceptedAt = -1;
    for (int i = 0; i < 11000; ++i)
    {
        if (!tone(300.0)) return 6;
        if (acceptedAt < 0 && dsp.getTargetMidi() == 62) acceptedAt = i;
    }
    if (acceptedAt < 0 || acceptedAt > (int) (0.085 * kRate))
    {
        std::fprintf(stderr,"new note never accepted or slow (%d samples)\n",acceptedAt);
        return 7;
    }

    // Broadband plosive/consonant burst: no stale voiced target after release.
    uint32_t seed = 0x1234ABCDu;
    double inputEnergy = 0.0, outputEnergy = 0.0;
    for (int i = 0; i < 1600; ++i)
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        const float n = 0.24f * ((float)(seed & 65535u) / 32767.5f - 1.0f);
        const float y = dsp.process(n);
        if (!std::isfinite(y) || std::abs(y) > 4.0f) return 8;
        if (i >= 800) { inputEnergy += n*n; outputEnergy += y*y; }
    }
    const double levelRatio = std::sqrt(outputEnergy / std::max(1.0e-20,inputEnergy));
    if (levelRatio < 0.75 || levelRatio > 1.3)
    {
        std::fprintf(stderr,"consonant level damaged: RMS ratio %.3f\n",levelRatio);
        return 10;
    }
    if (dsp.isVoiced() || std::abs(dsp.getCorrectionSemitones()) > 0.001f)
    {
        std::fprintf(stderr,"consonant retained voiced pitch state\n"); return 9;
    }
    std::printf("PASS confidence confirmation: rejected 8ms false note (max shift %.2fst); accepted real note in %.1fms; unvoiced release OK; consonant RMS ratio %.3f\n",largestSpuriousCorrection,(double)acceptedAt * 1000.0/kRate,levelRatio);
    return 0;
}
