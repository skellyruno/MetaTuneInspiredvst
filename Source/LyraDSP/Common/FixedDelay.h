#pragma once
#include <algorithm>
#include <vector>

namespace lyra
{
// Integer-sample delay used to keep dry/bypass paths aligned with the reported latency.
// A delay change crossfades from the old to the new read position over `fadeSamples`
// (an instant jump would click). prepare() allocates; setDelay()/process() are RT-safe, in place.
class FixedDelay
{
public:
    void prepare (int numChannels, int maxDelaySamples, int fadeSamples = 0)
    {
        len = std::max (0, maxDelaySamples) + 1;
        buf.assign (size_t (std::max (1, numChannels)), std::vector<float> (size_t (len), 0.0f));
        delay = oldDelay = len - 1;
        fadeLen = std::max (0, fadeSamples);
        fadePos = fadeLen;
        pos = 0;
    }
    void setDelay (int samples) noexcept
    {
        samples = std::clamp (samples, 0, len - 1);
        if (samples == delay) return;
        oldDelay = fadePos < fadeLen ? oldDelay : delay; // mid-fade retarget: keep fading from the original
        delay = samples;
        fadePos = 0;
    }
    void reset() noexcept { for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f); pos = 0; fadePos = fadeLen; }

    void process (float* const* ch, int numCh, int n) noexcept
    {
        numCh = std::min (numCh, int (buf.size()));
        for (int i = 0; i < n; ++i)
        {
            const int rd = (pos - delay + len) % len;
            const bool fading = fadePos < fadeLen;
            const int rdOld = (pos - oldDelay + len) % len;
            const float g = fading ? float (fadePos) / float (fadeLen) : 1.0f;
            for (int c = 0; c < numCh; ++c)
            {
                auto& b = buf[size_t (c)];
                b[size_t (pos)] = ch[c][i];
                ch[c][i] = fading ? g * b[size_t (rd)] + (1.0f - g) * b[size_t (rdOld)] : b[size_t (rd)];
            }
            fadePos += fading;
            pos = (pos + 1) % len;
        }
    }

private:
    std::vector<std::vector<float>> buf;
    int len = 1, delay = 0, oldDelay = 0, pos = 0, fadeLen = 0, fadePos = 0;
};
} // namespace lyra
