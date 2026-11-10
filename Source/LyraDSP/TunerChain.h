#pragma once
#include "Common/FixedDelay.h"
#include "Common/SpscQueue.h"
#include "Doubler.h"
#include "FormantAccent.h"
#include "Pitch/CorrectionEngine.h"
#include "Pitch/PitchShifter.h"
#include "Pitch/YinDetector.h"

namespace lyra
{
// The complete real-time tuner signal path, independent of any plugin framework so the plugin and
// the offline render/test tools run identical code:
//   mono sum -> YinDetector -> CorrectionEngine -> PitchShifter (stereo-linked) -> FormantAccent -> Doubler
//   HQ mode: the shifter's input is delayed by kLookaheadSeconds while the detector hears it
//   undelayed, so corrections land on time (adds that much reported latency).
//   dry path delayed by the same total latency; bypass = 10 ms linear crossfade between the two.
//
// One display sample per detector hop, sent to the UI through a lock-free queue.
struct Telemetry
{
    float inputMidi = 0;  // fractional MIDI of the detected pitch; 0 = unvoiced
    float outputMidi = 0; // inputMidi + applied correction
    float confidence = 0;
    float rms = 0;
    int target = -1;      // target MIDI note, -1 = none
    int pending = -1;     // note waiting for Note Stabilizer confirmation, -1 = none
};
using TelemetryQueue = SpscQueue<Telemetry, 1024>; // ~2 s at the 2 ms hop

// Threading: prepare() allocates; everything else is RT-safe and audio-thread owned.
class TunerChain
{
public:
    static constexpr int kSubBlock = 32; // control-rate granularity (0.67 ms @48 kHz)
    static constexpr int kMaxChannels = 2;
    static constexpr double kLookaheadSeconds = 0.002; // HQ mode
    static_assert (kSubBlock <= Doubler::kMaxBlock);

    struct Settings
    {
        CorrectionEngine::Params correction;
        double minFreq = 70.0; // range floor: sets detector frame and shifter latency
        bool hq = false;       // 2 ms detector lookahead (+2 ms latency)
        bool bypass = false;
        Doubler::Params doubler;
        bool strike = false;   // LyraStrike formant accent on note changes
        double strikeDepth = FormantAccent::kDefaultDepth;   // its formant shift (0.01..0.25)
        double strikeLength = FormantAccent::kDefaultLength; // its accent length (0.025..0.1 s)
    };

    void prepare (double sampleRate, double lowestMinFreq);
    void reset() noexcept;
    void setSettings (const Settings&) noexcept; // call once per block
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    int latencySamples() const noexcept { return shifter.latencySamples() + (hq ? lookahead : 0); }
    static int lookaheadFor (double sampleRate) noexcept { return int (std::lround (kLookaheadSeconds * sampleRate)); }
    static int latencyFor (double sampleRate, double minFreq, bool hq = false) noexcept
    {
        return PitchShifter::latencyFor (sampleRate, minFreq) + (hq ? lookaheadFor (sampleRate) : 0);
    }

    // Optional; the queue must outlive the chain. Set before processing starts.
    void setTelemetryQueue (TelemetryQueue* q) noexcept { telemetry = q; }

    const CorrectionEngine& correction() const noexcept { return engine; }
    const YinDetector& detector() const noexcept { return yin; }
    const FormantAccent& formantAccent() const noexcept { return strike; }

private:
    YinDetector yin;
    CorrectionEngine engine;
    PitchShifter shifter;
    FixedDelay dryDelay, aheadDelay;
    Doubler doubler;
    FormantAccent strike;
    int strikeLastNote = -1, strikeSilentHops = 0; // previous note for LyraStrike's direction
    int lookahead = 0;
    bool hq = false;
    TelemetryQueue* telemetry = nullptr;
    double sr = 48000, minFreq = 70;
    float wet = 1, wetTarget = 1, wetStep = 0.01f;
    float mono[kSubBlock] {}, ratio[kSubBlock] {}, dry[kMaxChannels][kSubBlock] {};
};
} // namespace lyra
