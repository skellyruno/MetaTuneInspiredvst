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
#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
    // Pre-allocate both period-splicing channels OUTSIDE the audio callback.
    // The original Lyra high-register renderer remains unchanged.
#if defined(SKELLTUNE_3WAY_AB_LAB) && SKELLTUNE_3WAY_AB_LAB
    const int nominalLiveDelay = PitchShifter::latencyFor(sr, 160.0);
    abVariant = 0;
    activeLiveFloorHz = 160.0;
#else
    const int nominalLiveDelay = PitchShifter::latencyFor(sr, kUltraLiveFloorHz);
#endif
    for (auto& tap : lowVoiceTap)
        tap.prepare(sr, nominalLiveDelay);
    lowVoiceTap[1].setFollower(true);
    lowVoiceAttackCoeff = float(1.0 - std::exp(-1.0/(0.008*sr)));
    lowVoiceReleaseCoeff = float(1.0 - std::exp(-1.0/(0.022*sr)));
    // Retire the old low-register period more quickly after two confident
    // *high* note estimates. Brief unvoiced gaps retain the slower grace.
    lowVoiceHighExitCoeff = float(1.0 - std::exp(-1.0/(0.006*sr)));
    lowVoiceGraceSamples = int(std::lround(sr*0.010));
#endif
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
    fastMixCoeff = float(1.0 - std::exp(-1.0 / (0.008 * sr)));
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
    stereoFallbackChannel = 0;
    wet = wetTarget;
    fastCorrectionMix = fastCorrectionTarget = ultraLive ? 0.0f : 1.0f;
    lastAppliedRatio = 1.0f;
#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
    for (auto& tap:lowVoiceTap) tap.reset();
    lowVoiceMix = lowVoiceTarget = 0.0f;
    lowVoiceLastPeriod = lowVoiceLastFreq = 0.0f;
    lowVoiceHoldRemaining = 0;
    lowVoiceHighRegisterHops = 0;
    lowVoiceFastRelease = false;
#endif
}

void TunerChain::setSettings (const Settings& s) noexcept
{
    engine.setParams (s.correction);
#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
    // With Amount zero the only correct output is a sample-exact dry delay.
    // Do not enable a second waveform tap just to reconstruct unity pitch.
#if defined(SKELLTUNE_3WAY_AB_LAB) && SKELLTUNE_3WAY_AB_LAB
    const int newVariant = std::clamp(s.abVariant, 0, 2);
    const double newFloorHz = newVariant == 2 ? 230.0 : 160.0;
    const bool floorChanged = newFloorHz != activeLiveFloorHz;
    if (newVariant != abVariant)
    {
        abVariant = newVariant;
        // Retime both linked taps without allocating. The next active render
        // re-enters through a dry-to-wet fade; old period evidence is reset.
        const int nextNominal = PitchShifter::latencyFor(sr, newFloorHz);
        for (auto& tap:lowVoiceTap) tap.setNominalDelaySamples(nextNominal);
        lowVoiceHoldRemaining = 0;
        lowVoiceLastPeriod = lowVoiceLastFreq = 0.0f;
        lowVoiceTarget = 0.0f;
    }
    activeLiveFloorHz = newFloorHz;
    lowVoiceEnabled = (abVariant != 0) && s.experimentalLowVoice && s.ultraLive && !s.hq
#else
    const bool floorChanged = false;
    lowVoiceEnabled = s.experimentalLowVoice && s.ultraLive && !s.hq
#endif
        && s.correction.amount > 1.0e-6;
    if (!lowVoiceEnabled)
        lowVoiceTarget = 0.0f;
#endif
#if !defined(SKELLTUNE_LOW_VOICE_LAB) || !SKELLTUNE_LOW_VOICE_LAB
    const bool floorChanged = false;
#endif
    if (s.minFreq != minFreq || s.hq != hq || s.ultraLive != ultraLive || floorChanged)
    {
        if (s.minFreq != minFreq)
        {
            minFreq = s.minFreq;
            yin.setMinFreq (minFreq);
            shifter.setMinFreq (minFreq);
        }
        hq = s.hq;
        ultraLive = s.ultraLive;
#if defined(SKELLTUNE_3WAY_AB_LAB) && SKELLTUNE_3WAY_AB_LAB
        shifter.setMinFreq(ultraLive ? std::max(minFreq, activeLiveFloorHz) : minFreq);
#else
        shifter.setMinFreq(ultraLive ? std::max(minFreq, kUltraLiveFloorHz) : minFreq);
#endif
        if (!ultraLive) fastCorrectionTarget = 1.0f;
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

    for (int start = 0; start < total; start += kSubBlock)
    {
        const int n = std::min (kSubBlock, total - start);
        float* io[kMaxChannels] {};
        float* dryPtr[kMaxChannels] { dry[0], dry[1] };
        for (int c = 0; c < numCh; ++c)
        {
            io[c] = ch[c] + start;
            // Sanitize the original source before it enters ANY filter or
            // delay ring. YIN alone rejecting NaNs cannot protect the shifter.
            for (int i = 0; i < n; ++i)
                if (!std::isfinite(io[c][i]) || std::abs(io[c][i]) > 32.0f
                    || std::abs(io[c][i]) < 1.0e-25f)
                    io[c][i] = 0.0f;
            std::copy (io[c], io[c] + n, dry[c]);
        }

        // Stereo-linked detection: one pitch decision for both channels.
        // Averaging anti-phase stereo vocals can erase the fundamental even
        // though the listener still hears both channels. If the coherent sum
        // nearly cancels, analyze the stronger single side instead; do not
        // alter the stereo audio or create a second pitch-control clock.
        bool antiphase = false;
        if (numCh == 2)
        {
            double leftPower = 0.0, rightPower = 0.0, sumPower = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double l = io[0][i], r = io[1][i];
                leftPower += l * l;
                rightPower += r * r;
                sumPower += (l + r) * (l + r);
            }
            antiphase = leftPower + rightPower > 1.0e-12
                        && sumPower < 0.15 * (leftPower + rightPower);
            // Only switch analysis sides for clear relative level changes.
            if (leftPower > 1.45 * rightPower) stereoFallbackChannel = 0;
            else if (rightPower > 1.45 * leftPower) stereoFallbackChannel = 1;
        }
        for (int i = 0; i < n; ++i)
            mono[i] = antiphase ? io[stereoFallbackChannel][i]
                               : (numCh == 2 ? 0.5f * (io[0][i] + io[1][i])
                                             : io[0][i]);
        yin.process (mono, n, [this] (int i, const PitchEstimate& e) {
            engine.onEstimate (e, i);
            shifter.setPeriod (e.voiced ? sr / e.frequency : 0.0);
#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
            // Old LAB hard-gated this path at .70 confidence even though
            // Skell Live accepts voiced notes at .65. Low/breathy vowels then
            // toggled between phase-different audio renderers each hop.
            // Use matched entry threshold and hysteretic 10ms grace for
            // dropouts; do not modify the proven production detector.
#if defined(SKELLTUNE_3WAY_AB_LAB) && SKELLTUNE_3WAY_AB_LAB
            const double lowVoiceEligibilityTopHz = abVariant == 2 ? 252.0 : 175.0;
#elif defined(SKELLTUNE_LAB_FAST_2P5) && SKELLTUNE_LAB_FAST_2P5
            // In the fast prototype Lyra becomes safe at 230Hz; the low
            // period-splice branch MUST remain eligible through 252Hz or
            // we leave an audible natural-pitch gap at 175..230Hz.
            constexpr double lowVoiceEligibilityTopHz =
                kUltraLiveFloorHz + 22.0;
#else
            constexpr double lowVoiceEligibilityTopHz = 175.0;
#endif
            const bool lowRange = e.voiced && e.frequency >= minFreq
                && e.frequency < lowVoiceEligibilityTopHz;
            const bool reliable = lowRange && e.confidence >=
                (lowVoiceHoldRemaining > 0 ? 0.58f : 0.65f);
            // A clear, sustained jump out of the low register is NOT a
            // transient detector dropout. Never hold a G#2 period under a
            // new high-register vowel for the full dropout grace period.
            // Two strong YIN hops prevent one isolated octave alias from
            // immediately changing the renderer.
            const bool strongHigh = e.voiced
                && e.frequency >= lowVoiceEligibilityTopHz
                && e.confidence >= 0.72f;
            if (reliable)
            {
                lowVoiceHighRegisterHops = 0;
                lowVoiceFastRelease = false;
                lowVoiceLastFreq = e.frequency;
                lowVoiceLastPeriod = float(sr / e.frequency);
                lowVoiceHoldRemaining = lowVoiceGraceSamples;
            }
            else if (strongHigh)
            {
                lowVoiceHighRegisterHops = std::min(2, lowVoiceHighRegisterHops + 1);
                if (lowVoiceHighRegisterHops >= 2)
                {
                    lowVoiceHoldRemaining = 0;
                    lowVoiceLastFreq = lowVoiceLastPeriod = 0.0f;
                    lowVoiceFastRelease = true;
                }
                else
                    lowVoiceHoldRemaining = std::max(0,
                        lowVoiceHoldRemaining - yin.hopSamples());
            }
            else
            {
                lowVoiceHighRegisterHops = 0;
                lowVoiceHoldRemaining = std::max(0,
                    lowVoiceHoldRemaining - yin.hopSamples());
            }
            const bool held = lowVoiceHoldRemaining > 0
                && lowVoiceLastPeriod > 0.0f;
            for(auto& tap:lowVoiceTap)
                tap.setPeriod(held ? lowVoiceLastPeriod : 0.0f);
            const float crossoverFreq = reliable ? e.frequency : lowVoiceLastFreq;
#if defined(SKELLTUNE_3WAY_AB_LAB) && SKELLTUNE_3WAY_AB_LAB
            const double lowVoiceWeight = abVariant == 2
                ? std::clamp((252.0 - double(crossoverFreq)) / 22.0, 0.0, 1.0)
                : std::clamp((170.0 - double(crossoverFreq)) / 25.0, 0.0, 1.0);
#elif defined(SKELLTUNE_LAB_FAST_2P5) && SKELLTUNE_LAB_FAST_2P5
            // 2.5ms Lyra read heads cannot cover 170..230Hz safely.
            // Keep the causal low period renderer fully active through
            // 230Hz and crossfade only where the faster Lyra history is
            // actually sufficient. No frequency gap can silently pass dry.
            const double lowVoiceWeight = std::clamp(
                (kUltraLiveFloorHz + 22.0-double(crossoverFreq))/22.0,0.0,1.0);
#else
            const double lowVoiceWeight = std::clamp(
                (170.0-double(crossoverFreq))/25.0,0.0,1.0);
#endif
            lowVoiceTarget = lowVoiceEnabled && held
                ? float(lowVoiceWeight) : 0.0f;
#endif
            // 3.5ms cannot span a complete 70Hz cycle. In Ultra Live only,
            // taper the requested ratio toward unity from 170 to 155 Hz.
            // The low voice stays audible with correct PDC and natural pitch.
            fastCorrectionTarget = !ultraLive ? 1.0f :
                (e.voiced ? float(std::clamp(
#if defined(SKELLTUNE_3WAY_AB_LAB) && SKELLTUNE_3WAY_AB_LAB
                    (double(e.frequency) - (activeLiveFloorHz - 5.0)) / 15.0,
#else
                    (double(e.frequency) - (kUltraLiveFloorHz - 5.0)) / 15.0,
#endif
                    0.0, 1.0)) : 0.0f);
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
        {
            fastCorrectionMix += (fastCorrectionTarget - fastCorrectionMix) * fastMixCoeff;
            const float requested = engine.nextRatio();
#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
            lowVoiceRatio[i] = std::clamp(requested,
                float(1.0/PitchShifter::kMaxRatio),
                float(PitchShifter::kMaxRatio));
            const float slew = lowVoiceTarget > lowVoiceMix
                ? lowVoiceAttackCoeff
                : (lowVoiceFastRelease ? lowVoiceHighExitCoeff : lowVoiceReleaseCoeff);
            lowVoiceMix += (lowVoiceTarget-lowVoiceMix)*slew;
            lowVoiceMix = std::clamp(lowVoiceMix,0.0f,1.0f);
            lowVoiceBlend[i] = lowVoiceMix;
#endif
            // The shifter can only render ratios in [1/1.5, 1.5].
            // Match its physical limit *before* publishing the output ratio:
            // a large Glide jump must not make the meter claim an octave
            // of correction when the actual read head caps near 7 semitones.
            ratio[i] = std::clamp(1.0f + fastCorrectionMix * (requested - 1.0f),
                                  float(1.0 / PitchShifter::kMaxRatio),
                                  float(PitchShifter::kMaxRatio));
#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
            // Meter is an approximation in transition because two
            // waveforms are mixed; fully low = the actual low-shifter ratio.
            lastAppliedRatio = ratio[i] + lowVoiceBlend[i]*(lowVoiceRatio[i]-ratio[i]);
#else
            lastAppliedRatio = ratio[i];
#endif
        }

#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
        // Two independent audio paths are fed the SAME source, and both
        // low taps share exactly one splice clock. Each tap's ring stays
        // warm even in high-register or Quality mode.
        for (int i=0;i<n;++i)
        {
            // Finish fade-out on mode automation rather than instantly
            // returning the low tap to unity/dry mid-cycle.
            const bool renderLow = ultraLive
                && (lowVoiceTarget>0.001f || lowVoiceBlend[i]>0.001f);
            lowVoiceTap[0].setRenderingEnabled(renderLow);
            lowVoiceTap[1].setRenderingEnabled(renderLow);
            for (auto& tap:lowVoiceTap) tap.setRatio(lowVoiceRatio[i]);
            lowVoiceSamples[0][i]=lowVoiceTap[0].processSample(io[0][i]);
            if(numCh>1)
            {
                lowVoiceTap[1].applyEvent(lowVoiceTap[0].getLastEvent());
                lowVoiceSamples[1][i]=lowVoiceTap[1].processSample(io[1][i]);
            }
        }
#endif
        aheadDelay.process (io, numCh, n); // HQ lookahead (0 samples when off)
        shifter.process (io, numCh, n, ratio);
#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
        // Lab-only adaptive crossfade. It may reveal comb filtering if the
        // low tap deviates from its nominal delay: quantify before release.
        for (int i=0;i<n;++i)
            for (int c=0;c<numCh;++c)
                io[c][i] += lowVoiceBlend[i]*(lowVoiceSamples[c][i]-io[c][i]);
#endif
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
