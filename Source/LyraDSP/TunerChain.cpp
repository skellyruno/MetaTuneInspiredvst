#include "TunerChain.h"
#include <algorithm>
#include <cmath>

namespace lyra
{
void TunerChain::prepare (double sampleRate, double lowestMinFreq)
{
    sr = sampleRate;
    minFreq = lowestMinFreq;
    YinDetector::Config cfg;
    cfg.minFreq = lowestMinFreq;
    yin.prepare (sr, cfg);
    engine.prepare (sr, yin.hopSamples() / sr);
    shifter.prepare (sr, kMaxChannels, lowestMinFreq);
    lookahead = lookaheadFor (sr);
    // Sized for the largest latency (lowest range + HQ); the active delays follow the settings.
    dryDelay.prepare (kMaxChannels, shifter.latencySamples() + lookahead, int (0.005 * sr));
    aheadDelay.prepare (kMaxChannels, lookahead, int (0.005 * sr));
    aheadDelay.setDelay (0);
    hq = false;
    dryDelay.setDelay (latencySamples());
    engine.setTiming (yin.windowSeconds(), latencySamples() / sr);
    doubler.prepare (sr);
    strike.prepare (sr);
    wetStep = float (1.0 / (0.010 * sr));
    reset();
}

void TunerChain::reset() noexcept
{
    yin.reset();
    engine.reset();
    shifter.reset();
    dryDelay.reset();
    aheadDelay.reset();
    doubler.reset();
    strike.reset();
    strikeLastNote = -1;
    strikeSilentHops = 0;
    wet = wetTarget;
}

void TunerChain::setSettings (const Settings& s) noexcept
{
    engine.setParams (s.correction);
    if (s.minFreq != minFreq || s.hq != hq)
    {
        if (s.minFreq != minFreq)
        {
            minFreq = s.minFreq;
            yin.setMinFreq (minFreq);
            shifter.setMinFreq (minFreq);
        }
        hq = s.hq;
        aheadDelay.setDelay (hq ? lookahead : 0); // crossfaded inside FixedDelay: no click
        dryDelay.setDelay (latencySamples());
        engine.setTiming (yin.windowSeconds(), latencySamples() / sr);
    }
    doubler.setParams (s.doubler);
    strike.setEnabled (s.strike);
    strike.setDepth (s.strikeDepth);
    strike.setLength (s.strikeLength);
    wetTarget = s.bypass ? 0.0f : 1.0f;
}

void TunerChain::process (float* const* ch, int numCh, int total) noexcept
{
    numCh = std::clamp (numCh, 0, kMaxChannels);
    if (numCh == 0 || total <= 0)
        return;
    const float invCh = 1.0f / float (numCh);

    for (int start = 0; start < total; start += kSubBlock)
    {
        const int n = std::min (kSubBlock, total - start);
        float* io[kMaxChannels] {};
        float* dryPtr[kMaxChannels] { dry[0], dry[1] };
        for (int c = 0; c < numCh; ++c)
        {
            io[c] = ch[c] + start;
            std::copy (io[c], io[c] + n, dry[c]);
        }

        // Stereo-linked detection: one pitch decision for both channels.
        for (int i = 0; i < n; ++i)
        {
            float s = 0;
            for (int c = 0; c < numCh; ++c) s += io[c][i];
            mono[i] = s * invCh;
        }
        yin.process (mono, n, [this] (int i, const PitchEstimate& e) {
            engine.onEstimate (e, i);
            shifter.setPeriod (e.voiced ? sr / e.frequency : 0.0);
            doubler.setPeriod (e.voiced ? sr / e.frequency : 0.0);
            // LyraStrike: accent each note-to-note change. Pending (stabilizer) or short consonant gaps
            // keep the previous note; >= 60 ms unvoiced starts a new phrase (no accent on its first note).
            if (const int note = engine.targetNote(); note >= 0)
            {
                if (strikeLastNote >= 0 && note != strikeLastNote)
                    strike.trigger (note > strikeLastNote ? 1 : -1);
                strikeLastNote = note;
            }
            if (engine.inputMidi() > 0)
                strikeSilentHops = 0;
            else if (++strikeSilentHops * yin.hopSamples() >= CorrectionEngine::kOnsetGapSeconds * sr)
                strikeLastNote = -1;
            if (telemetry != nullptr)
            {
                Telemetry t;
                t.inputMidi = float (engine.inputMidi());
                // The applied correction (smoothed) is what the listener hears, not the requested one.
                t.outputMidi = t.inputMidi > 0 ? float (engine.inputMidi() + engine.correctionSemitones()) : 0.0f;
                t.confidence = e.confidence;
                t.rms = e.rms;
                t.target = engine.targetNote();
                t.pending = engine.pendingNote();
                telemetry->push (t); // drops when the UI is not draining (editor closed)
            }
        });
        for (int i = 0; i < n; ++i)
            ratio[i] = engine.nextRatio();

        aheadDelay.process (io, numCh, n); // HQ lookahead (0 samples when off)
        shifter.process (io, numCh, n, ratio);
        strike.process (io, numCh, n); // before the doubler: the doubled voices carry the accent too
        doubler.process (io, numCh, n);
        dryDelay.process (dryPtr, numCh, n);

        for (int i = 0; i < n; ++i)
        {
            if (wet != wetTarget)
                wet = wet < wetTarget ? std::min (wetTarget, wet + wetStep) : std::max (wetTarget, wet - wetStep);
            for (int c = 0; c < numCh; ++c)
                io[c][i] = dry[c][i] + wet * (io[c][i] - dry[c][i]);
        }
    }
}
} // namespace lyra
