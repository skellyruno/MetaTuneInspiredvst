#include "Doubler.h"
#include <algorithm>
#include <cmath>

namespace lyra
{
static constexpr int kRing = 4096;

void Doubler::prepare (double sampleRate)
{
    sr = sampleRate;
    smooth = float (1.0 - std::exp (-1.0 / (0.020 * sr)));
    driftCoef = float (1.0 - std::exp (-1.0 / (0.150 * sr)));
    // Voice character: detune (cents), base timing (ms, on top of the shifter's centre delay), drift depth (ms).
    const double detune[kVoices] { -8.0, 6.0 }, timingMs[kVoices] { 8.0, 14.0 }, driftMs[kVoices] { 2.0, 2.5 };
    const float pans[kVoices] { -1.0f, 1.0f };
    for (int v = 0; v < kVoices; ++v)
    {
        auto& vo = voices[v];
        vo.shifter.prepare (sr, 1, 70.0);
        vo.detuneCents = detune[v];
        // Keep base + drift inside the ring (which is sized for 48 kHz; high rates use proportionally less ms).
        vo.baseDelay = std::min (timingMs[v] * 0.001 * sr, double (kRing) * 0.5);
        vo.driftDepth = std::min (driftMs[v] * 0.001 * sr, double (kRing) * 0.2);
        vo.pan = pans[v];
        vo.rng = 0x9E3779B9u * std::uint32_t (v + 1);
    }
    reset();
}

void Doubler::reset() noexcept
{
    for (auto& vo : voices)
    {
        vo.shifter.reset();
        std::fill (std::begin (vo.ring), std::end (vo.ring), 0.0f);
        vo.writePos = 0;
        vo.driftT = vo.driftP = vo.targetT = vo.targetP = 0;
        vo.samplesToNext = 0;
    }
    gain = params.enabled ? params.blend : 0.0f;
    width = params.width;
}

void Doubler::setParams (const Params& p) noexcept
{
    params = p;
    params.width = std::isfinite (p.width) ? std::clamp (p.width, 0.0f, 1.0f) : 0.7f;
    params.blend = std::isfinite (p.blend) ? std::clamp (p.blend, 0.0f, 1.0f) : 0.5f;
}

void Doubler::setPeriod (double samples) noexcept
{
    for (auto& vo : voices) vo.shifter.setPeriod (samples);
}

float Doubler::nextRandom (Voice& vo) noexcept
{
    vo.rng = vo.rng * 1664525u + 1013904223u; // LCG: deterministic, allocation-free
    return float (vo.rng >> 8) / float (1u << 24) * 2.0f - 1.0f;
}

float Doubler::readDelay (const Voice& vo, double delay) const noexcept
{
    // Linear interpolation is enough here: the delay only moves a few samples per second.
    const double pos = double (vo.writePos) - delay;
    const double fl = std::floor (pos);
    const float t = float (pos - fl);
    const int i = int (fl);
    const float a = vo.ring[(i + kRing) & (kRing - 1)], b = vo.ring[(i + 1 + kRing) & (kRing - 1)];
    return a + t * (b - a);
}

void Doubler::process (float* const* ch, int numCh, int n) noexcept
{
    n = std::min (n, kMaxBlock);
    numCh = std::min (numCh, 2);
    if (numCh <= 0 || n <= 0) return;
    if (! params.enabled && gain <= 1e-4f) { gain = 0; return; } // fully off: untouched, zero cost

    for (int i = 0; i < n; ++i)
    {
        float s = 0;
        for (int c = 0; c < numCh; ++c) s += ch[c][i];
        mono[i] = s / float (numCh);
    }

    // Per-voice: timing delay with drift, then detuned + wobbling pitch shift.
    for (auto& vo : voices)
    {
        for (int i = 0; i < n; ++i)
        {
            if (--vo.samplesToNext <= 0)
            {
                vo.targetT = nextRandom (vo);
                vo.targetP = nextRandom (vo);
                vo.samplesToNext = int ((0.3 + 0.3 * (0.5 + 0.5 * nextRandom (vo))) * sr);
            }
            vo.driftT += driftCoef * (vo.targetT - vo.driftT);
            vo.driftP += driftCoef * (vo.targetP - vo.driftP);
            vo.ring[vo.writePos] = mono[i];
            vo.buf[i] = readDelay (vo, vo.baseDelay + vo.driftDepth * (1.0 + vo.driftT));
            vo.writePos = (vo.writePos + 1) & (kRing - 1);
            vo.ratio[i] = float (std::exp2 ((vo.detuneCents + 3.0 * vo.driftP) / 1200.0)); // +-3 c wobble
        }
        float* p = vo.buf;
        vo.shifter.process (&p, 1, n, vo.ratio);
    }

    // Mix with smoothed enable/blend/width; constant-power pan.
    const float target = params.enabled ? params.blend : 0.0f;
    for (int i = 0; i < n; ++i)
    {
        gain += smooth * (target - gain);
        width += smooth * (params.width - width);
        const float mainGain = 1.0f - 0.3f * gain, voiceGain = 0.75f * gain;
        for (int c = 0; c < numCh; ++c) ch[c][i] *= mainGain;
        for (auto& vo : voices)
        {
            const float x = vo.buf[i] * voiceGain;
            if (numCh == 1) { ch[0][i] += x * 0.7071f; continue; }
            const float angle = (vo.pan * width + 1.0f) * 0.78539816f; // 0..pi/2
            ch[0][i] += x * std::cos (angle);
            ch[1][i] += x * std::sin (angle);
        }
    }
}
} // namespace lyra
