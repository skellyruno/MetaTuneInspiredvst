// offline_render: run a WAV through the Skelly engine in Quality, Live or Snap Live mode.
// Build: g++ -std=c++17 -O2 -ISource tools/offline_render.cpp -o offline_render
// Usage: offline_render in.wav out.wav [--mode live|snap|quality] [--retune ms] [--block n]
//        offline_render --synth out.wav        (writes a detuned test vowel glide, 48 kHz mono)
// Reads PCM16/PCM24/float32 WAV (mono or stereo); writes float32 WAV with the input's channel count.
// The output is NOT latency-compensated, so for A/B in an editor align by the mode's reported latency.
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace autotune;

struct Wav { int ch = 1; double sr = 48000; std::vector<std::vector<float>> d; };

static bool readWav (const char* path, Wav& w)
{
    FILE* f = std::fopen (path, "rb"); if (! f) return false;
    std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
    while ((n = std::fread (buf, 1, sizeof buf, f)) > 0) b.insert (b.end(), buf, buf + n);
    std::fclose (f);
    if (b.size() < 44 || std::memcmp (b.data(), "RIFF", 4) || std::memcmp (b.data() + 8, "WAVE", 4)) return false;
    int fmt = 0, bits = 0; size_t pos = 12, dpos = 0, dlen = 0;
    auto u16 = [&] (size_t p) { return (int) (b[p] | b[p + 1] << 8); };
    auto u32 = [&] (size_t p) { return (uint32_t) (b[p] | b[p + 1] << 8 | b[p + 2] << 16 | (uint32_t) b[p + 3] << 24); };
    while (pos + 8 <= b.size())
    {
        const uint32_t len = u32 (pos + 4);
        if (! std::memcmp (&b[pos], "fmt ", 4)) { fmt = u16 (pos + 8); w.ch = u16 (pos + 10); w.sr = u32 (pos + 12); bits = u16 (pos + 22); }
        if (! std::memcmp (&b[pos], "data", 4)) { dpos = pos + 8; dlen = std::min<size_t> (len, b.size() - dpos); break; }
        pos += 8 + len + (len & 1);
    }
    if (w.ch < 1 || w.ch > 2 || ! dpos) return false;
    const int bytes = bits / 8; if (bytes < 2) return false;
    const bool isFloat = fmt == 3 || (fmt == 0xFFFE && bits == 32);
    if (! isFloat && bits != 16 && bits != 24) return false;
    const size_t frames = dlen / ((size_t) bytes * (size_t) w.ch);
    w.d.assign ((size_t) w.ch, std::vector<float> (frames));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < w.ch; ++c)
        {
            const uint8_t* p = &b[dpos + (i * (size_t) w.ch + (size_t) c) * (size_t) bytes];
            float v;
            if (isFloat) { uint32_t u = (uint32_t) (p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24); std::memcpy (&v, &u, 4); }
            else if (bits == 16) v = (int16_t) (p[0] | p[1] << 8) / 32768.0f;
            else v = ((int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24) >> 8) / 8388608.0f;
            w.d[(size_t) c][i] = v;
        }
    return true;
}

static bool writeWav (const char* path, const Wav& w)
{
    FILE* f = std::fopen (path, "wb"); if (! f) return false;
    const uint32_t frames = (uint32_t) w.d[0].size(), dl = frames * (uint32_t) w.ch * 4, sr = (uint32_t) w.sr;
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dl); std::fwrite ("WAVEfmt ", 1, 8, f); w32 (16); w16 (3); w16 ((uint16_t) w.ch);
    w32 (sr); w32 (sr * (uint32_t) w.ch * 4); w16 ((uint16_t) (w.ch * 4)); w16 (32); std::fwrite ("data", 1, 4, f); w32 (dl);
    for (uint32_t i = 0; i < frames; ++i) for (int c = 0; c < w.ch; ++c) std::fwrite (&w.d[(size_t) c][i], 4, 1, f);
    std::fclose (f); return true;
}

int main (int argc, char** argv)
{
    if (argc >= 3 && ! std::strcmp (argv[1], "--synth"))
    {
        Wav w; w.sr = 48000; w.d.assign (1, std::vector<float> ((size_t) (w.sr * 6)));
        double ph = 0; const double notes[] = { 110.0, 123.47, 146.83, 130.81, 110.0, 164.81 };
        for (size_t i = 0; i < w.d[0].size(); ++i)
        {
            const double t = (double) i / w.sr;
            const double f = notes[(int) (t / 1.0)] * std::exp2 ((-0.30 + 0.12 * std::sin (6.2832 * 5.5 * t)) / 12.0); // 30 cents flat + vibrato
            ph += 6.283185307 * f / w.sr;
            w.d[0][i] = (float) (0.22 * std::sin (ph) + 0.11 * std::sin (2 * ph + .3) + 0.07 * std::sin (3 * ph - .7) + 0.04 * std::sin (4 * ph + 1.1));
        }
        return writeWav (argv[2], w) ? 0 : 1;
    }
    if (argc < 3) { std::fprintf (stderr, "usage: offline_render in.wav out.wav [--mode live|snap|quality] [--retune ms] [--block n]\n       offline_render --synth out.wav\n"); return 2; }
    std::string mode = "snap"; float retune = 20.0f; int block = 128;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        if (! std::strcmp (argv[i], "--mode")) mode = argv[i + 1];
        else if (! std::strcmp (argv[i], "--retune")) retune = (float) std::atof (argv[i + 1]);
        else if (! std::strcmp (argv[i], "--block")) block = std::max (1, std::atoi (argv[i + 1]));
    }
    Wav in; if (! readWav (argv[1], in)) { std::fprintf (stderr, "cannot read %s (need PCM16/24 or float32 WAV, 1-2 ch)\n", argv[1]); return 1; }
    RealtimeAutotune e; e.prepare (in.sr);
    Settings s; s.strictRealtime = true; s.ultraLowLatency = true; s.lowLatency = mode != "quality"; s.snapLive = mode == "snap"; s.retuneMs = retune;
    e.setSettings (s);
    const size_t frames = in.d[0].size(); std::vector<float> intrinsic ((size_t) block * 2);
    for (size_t pos = 0; pos < frames; pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, frames - pos);
        float* ch[2] = { &in.d[0][pos], &in.d[(size_t) (in.ch - 1)][pos] };
        e.processChannels (ch, in.ch, n, intrinsic.data());
    }
    const int latency = mode == "snap" ? SnapLiveShifter::hostLatencySamples (in.sr)
                      : mode == "live" ? RealtimeAutotune::ultraLowLatencyHostSamples (in.sr) : RealtimeAutotune::realtimeQualityHostSamples (in.sr);
    std::printf ("mode %s, nominal latency %d samples (%.2f ms); output is not latency-compensated\n", mode.c_str(), latency, 1000.0 * latency / in.sr);
    return writeWav (argv[2], in) ? 0 : 1;
}
