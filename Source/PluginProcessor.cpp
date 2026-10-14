#include "PluginProcessor.h"
#include "PluginEditor.h"

MetaTuneInspiredAudioProcessor::MetaTuneInspiredAudioProcessor()
 : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
   parameters(*this, nullptr, "PARAMETERS", createParameters())
{
    speedParameter = parameters.getRawParameterValue("speed");
    amountParameter = parameters.getRawParameterValue("amount");
    sustainParameter = parameters.getRawParameterValue("sustain");
    scaleParameter = parameters.getRawParameterValue("scale");
    rootParameter = parameters.getRawParameterValue("root");
    lowLatencyParameter = parameters.getRawParameterValue("lowLatency");
    stabilizerParameter = parameters.getRawParameterValue("stabilizer");
    stabilizerTimeParameter = parameters.getRawParameterValue("stabilizerTime");
    pitchReferenceParameter = parameters.getRawParameterValue("pitchReference");
    doublerWidthParameter = parameters.getRawParameterValue("doublerWidth");
    doublerMixParameter = parameters.getRawParameterValue("doublerMix");
    globalBypassParameter = parameters.getRawParameterValue("globalBypass");
    toneVolumeParameter = parameters.getRawParameterValue("toneVolume");
}

float MetaTuneInspiredAudioProcessor::speedControlToMilliseconds(float controlValue) noexcept
{
    const float p = juce::jlimit(0.0f, 100.0f, controlValue);
    // 0 -> 200 ms; 30 -> 30 ms; 100 -> -3 ms, matching the user-facing dial.
    if (p <= 30.0f)
        return 200.0f - (170.0f * p / 30.0f);
    return 30.0f - (33.0f * (p - 30.0f) / 70.0f);
}

juce::AudioProcessorValueTreeState::ParameterLayout MetaTuneInspiredAudioProcessor::createParameters()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout p;
    p.add(std::make_unique<AudioParameterFloat>("speed", "Speed", NormalisableRange<float>(0.0f, 100.0f, 0.1f), 30.0f));
    p.add(std::make_unique<AudioParameterFloat>("amount", "Amount", NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    p.add(std::make_unique<AudioParameterFloat>("pitchReference", "Pitch Reference",
        NormalisableRange<float>(415.3047f, 466.1638f, 0.1f), 440.0f));
    // Signed values: negative slows retuning on held notes, positive speeds it up.
    p.add(std::make_unique<AudioParameterFloat>("sustain", "Sustain", NormalisableRange<float>(-100.0f, 100.0f, 1.0f), 0.0f));
    p.add(std::make_unique<AudioParameterBool>("stabilizer", "Note Stabilizer", true));
    p.add(std::make_unique<AudioParameterChoice>("stabilizerTime", "Note Stabilizer Time",
                                                  StringArray { "None", "Short", "Mid", "Long" }, 1));
    p.add(std::make_unique<AudioParameterBool>("lowLatency", "Low Latency", true));
    p.add(std::make_unique<AudioParameterChoice>("scale", "Scale", StringArray{"Chromatic","Major","Minor"}, 0));
    p.add(std::make_unique<AudioParameterInt>("root", "Root Note", 0, 11, 4));
    p.add(std::make_unique<AudioParameterFloat>("doublerWidth", "Doubler Width",
        NormalisableRange<float>(0.0f, 100.0f, 0.1f), 46.0f));
    p.add(std::make_unique<AudioParameterFloat>("doublerMix", "Doubler Mix",
        NormalisableRange<float>(0.0f, 100.0f, 0.1f), 49.0f));
    p.add(std::make_unique<AudioParameterBool>("globalBypass", "Global Bypass", false));
    p.add(std::make_unique<AudioParameterFloat>("toneVolume", "Tone Volume",
        NormalisableRange<float>(0.0f, 100.0f, 0.1f), 65.0f));
    return p;
}

void MetaTuneInspiredAudioProcessor::prepareToPlay(double sr, int maxBlock)
{
    juce::ignoreUnused(maxBlock);
    currentSampleRate = juce::jmax(8000.0, sr);

    // Pitch-tracking history is decimated to keep CPU predictable at 96/192 kHz.
    // 512 effective samples gives a ~10.7 ms frame at 48 kHz; the long path uses
    // 1024 effective samples for lower voices and more stable octave decisions.
    shortAnalysisDecimation = juce::jmax(1, juce::roundToInt((float)currentSampleRate / 48000.0f));
    mediumAnalysisDecimation = juce::jmax(1, juce::roundToInt((float)currentSampleRate / 32000.0f));
    longAnalysisDecimation = juce::jmax(2, juce::roundToInt((float)currentSampleRate / 24000.0f));
    shortAnalysisWindowSamples = 512 * shortAnalysisDecimation;
    mediumAnalysisWindowSamples = 768 * mediumAnalysisDecimation;
    longAnalysisWindowSamples = 1024 * longAnalysisDecimation;
    lastAnalysisLowLatencyMode = lowLatencyParameter != nullptr
        && lowLatencyParameter->load(std::memory_order_relaxed) > 0.5f;
    pitchAnalysisHopSamples = juce::jmax(64, juce::roundToInt((float)currentSampleRate
                                      * (lastAnalysisLowLatencyMode ? 0.004f : 0.008f)));

    // Fixed low-latency dual-head delay shifter. The delay lines are fixed-size
    // members, cleared here on the non-audio prepare path; no allocations occur
    // during processing. Report the nominal centre delay to the host.
    // A 10 ms grain gives vocals more room for stable overlap than the former 4 ms
    // window, while keeping centre-path latency near 6 ms at common sample rates.
    // Fixed capacity remains sufficient at 192 kHz (window <= 1920 samples).
    pitchShiftWindowSamples = juce::jlimit(256, 3072, juce::roundToInt((float)currentSampleRate * 0.010f));
    pitchShiftBaseDelaySamples = juce::jmax(8, juce::roundToInt((float)currentSampleRate * 0.001f));
    pitchShiftDelayLine = {};
    doublerDelayLine = {};
    pitchShiftWritePosition = 0;
    doublerWritePosition = 0;
    pitchShiftPhase = 0.25f;
    currentPitchRatio = 1.0f;
    pitchShiftWet = 0.0f;
    referenceTonePhase = 0.0f;
    referenceToneAmplitude = 0.0f;
    referenceToneFrequencyHz = pitchReferenceParameter != nullptr
        ? pitchReferenceParameter->load(std::memory_order_relaxed) : 440.0f;
    tonePreviewMidiNote.store(-1, std::memory_order_relaxed);
    doublerLfoPhase = 0.0f;
    smoothedDoublerWidth = doublerWidthParameter != nullptr
        ? juce::jlimit(0.0f, 1.0f, doublerWidthParameter->load(std::memory_order_relaxed) / 100.0f) : 0.46f;
    smoothedDoublerMix = doublerMixParameter != nullptr
        ? juce::jlimit(0.0f, 1.0f, doublerMixParameter->load(std::memory_order_relaxed) / 100.0f) : 0.49f;
    setLatencySamples(pitchShiftBaseDelaySamples + pitchShiftWindowSamples / 2);

    pitchHistory.fill(0.0f);
    pitchHistoryWritePosition = 0;
    pitchHistorySamples = 0;
    smoothedPitchChannelEnergy = { 0.0, 0.0 };
    selectedPitchAnalysisChannel = 0;
    samplesSincePitchAnalysis = 0;
    fallbackAnalysisCounter = 0;
    consecutiveUnvoicedAnalyses = 0;
    pendingOctaveJumpMidi = 69.0f;
    pendingOctaveJumpConfirmations = 0;
    detectedHz.store(0.0f, std::memory_order_relaxed);
    detectedMidiNote.store(69.0f, std::memory_order_relaxed);
    correctionCents.store(0.0f, std::memory_order_relaxed);
    pitchConfidence.store(0.0f, std::memory_order_relaxed);
    inputRms.store(0.0f, std::memory_order_relaxed);
    targetMidiNote.store(69.0f, std::memory_order_relaxed);
    smoothedTargetMidiNote.store(69.0f, std::memory_order_relaxed);
    effectiveRetuneTimeMs.store(30.0f, std::memory_order_relaxed);
    heldPitchAnchorMidi = 69.0f;
    heldPitchSamples = 0;
    audioSmoothedTargetMidi = 69.0f;
    audioSmoothedCorrectionSemitones = 0.0f;
    hasHeldPitchAnchor = false;
    hasSmoothedTarget = false;
    committedScaleNote = 69;
    pendingScaleNote = 69;
    pendingScaleSamples = 0;
    hasCommittedScaleNote = false;
    hasPendingScaleNote = false;
}

void MetaTuneInspiredAudioProcessor::releaseResources() {}

bool MetaTuneInspiredAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void MetaTuneInspiredAudioProcessor::updatePitchEstimate(const juce::AudioBuffer<float>& buffer)
{
    // Feed a fixed-size history from every callback, including tiny FL Studio/ASIO
    // blocks. Analyse the stronger input channel instead of averaging L/R, because
    // wide or polarity-inverted stereo vocals can cancel when naively downmixed.
    const int channels = juce::jmin(2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    std::array<double, 2> channelEnergy { 0.0, 0.0 };
    for (int channel = 0; channel < channels; ++channel)
    {
        const auto* input = buffer.getReadPointer(channel);
        double energy = 0.0;
        for (int i = 0; i < samples; ++i)
            energy += (double) input[i] * (double) input[i];
        channelEnergy[(size_t)channel] = energy;
    }

    // Smooth energy across callbacks and add hysteresis before changing channels.
    // Selecting the loudest channel independently on every small host block can
    // splice left/right waveforms into one pitch-history buffer, especially with
    // stereo effects or a moving vocal pan.
    const double energyAlpha = 1.0 - std::exp(-(double)samples
        / (juce::jmax(8000.0, currentSampleRate) * 0.045));
    for (int channel = 0; channel < channels; ++channel)
        smoothedPitchChannelEnergy[(size_t)channel] +=
            (channelEnergy[(size_t)channel] - smoothedPitchChannelEnergy[(size_t)channel]) * energyAlpha;

    const int previousAnalysisChannel = selectedPitchAnalysisChannel;
    if (channels > 1)
    {
        const int otherChannel = 1 - selectedPitchAnalysisChannel;
        const double currentEnergy = smoothedPitchChannelEnergy[(size_t)selectedPitchAnalysisChannel];
        const double otherEnergy = smoothedPitchChannelEnergy[(size_t)otherChannel];
        // Require a clear 2.5 dB advantage to avoid rapid L/R toggling.
        if (otherEnergy > currentEnergy * 1.778279410)
            selectedPitchAnalysisChannel = otherChannel;
    }
    else
        selectedPitchAnalysisChannel = 0;

    // Never splice a frame from left and right channels. A source switch invalidates
    // the old history; refill it from the newly selected channel before trusting pitch.
    if (selectedPitchAnalysisChannel != previousAnalysisChannel)
    {
        pitchHistoryWritePosition = 0;
        pitchHistorySamples = 0;
        samplesSincePitchAnalysis = 0;
        fallbackAnalysisCounter = 0;
        consecutiveUnvoicedAnalyses = 0;
        pendingOctaveJumpConfirmations = 0;
        pitchConfidence.store(0.0f, std::memory_order_relaxed);
        detectedHz.store(0.0f, std::memory_order_relaxed);
        correctionCents.store(0.0f, std::memory_order_relaxed);
    }

    const int analysisChannel = selectedPitchAnalysisChannel;
    const auto* analysis = buffer.getReadPointer(analysisChannel);
    for (int i = 0; i < samples; ++i)
    {
        const float sample = analysis[i];
        pitchHistory[(size_t) pitchHistoryWritePosition] = sample;
        pitchHistoryWritePosition = (pitchHistoryWritePosition + 1) % pitchHistorySize;
        pitchHistorySamples = juce::jmin(pitchHistorySize, pitchHistorySamples + 1);
        ++samplesSincePitchAnalysis;

        // The analysis hop is sample-rate-aware (about 4 ms), so tracking still
        // runs on hosts that deliver 32/64/128/256-sample callbacks.
        if (samplesSincePitchAnalysis >= pitchAnalysisHopSamples)
        {
            samplesSincePitchAnalysis -= pitchAnalysisHopSamples;
            estimatePitchFromHistory();
        }
    }

    const double strongestEnergy = channelEnergy[(size_t)analysisChannel];
    inputRms.store((float) std::sqrt(strongestEnergy / (double) samples), std::memory_order_relaxed);
}

void MetaTuneInspiredAudioProcessor::estimatePitchFromHistory() noexcept
{
    const bool lowLatency = lowLatencyParameter != nullptr
        && lowLatencyParameter->load(std::memory_order_relaxed) > 0.5f;
    // Quality mode may use the medium frame first; requiring the full bass frame here
    // added an unnecessary ~40 ms of startup silence before any estimate was attempted.
    const int initialWindowNeeded = lowLatency ? shortAnalysisWindowSamples : mediumAnalysisWindowSamples;
    ++fallbackAnalysisCounter;

    if (pitchHistorySamples < initialWindowNeeded || currentSampleRate < 8000.0)
        return;

    auto rejectFrame = [this]() noexcept
    {
        pendingOctaveJumpConfirmations = 0;
        // Decay confidence over a few analysis hops instead of dropping it to zero
        // on the first unvoiced/consonant frame. The processor can smoothly release
        // correction while the UI continues to show only high-confidence notes.
        const float previousConfidence = pitchConfidence.load(std::memory_order_relaxed);
        pitchConfidence.store(previousConfidence * 0.78f, std::memory_order_relaxed);
        if (++consecutiveUnvoicedAnalyses >= 3)
        {
            detectedHz.store(0.0f, std::memory_order_relaxed);
            correctionCents.store(0.0f, std::memory_order_relaxed);
        }
    };

    // The short frame is preferred in Low Latency mode. It uses 512 effective
    // samples (~10.7 ms at 48 kHz) for upper vocal notes; bass notes fall back to
    // a 1024-point decimated frame. This is analysis look-back, not buffer delay.
    const auto analyseWindow = [this](int sourceSamples, int decimation,
                                      float effectiveRate, float minimumFrequency,
                                      bool rejectEdgeMinimum,
                                      float& hzOut, float& confidenceOut) noexcept -> bool
    {
        const int n = juce::jlimit(256, 1024, sourceSamples / juce::jmax(1, decimation));
        constexpr int lagStorage = 512;
        constexpr float rmsFloor = 0.0025f;
        constexpr float yinThreshold = 0.18f;
        constexpr float confidenceFloor = 0.60f;
        if (pitchHistorySamples < sourceSamples)
            return false;

        std::array<float, 1024> frame {};
        const int first = (pitchHistoryWritePosition - sourceSamples + pitchHistorySize) % pitchHistorySize;
        double mean = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const int idx = (first + i * decimation) % pitchHistorySize;
            frame[(size_t) i] = pitchHistory[(size_t) idx];
            mean += frame[(size_t) i];
        }
        mean /= (double) n;

        double energy = 0.0;
        const int taperSamples = juce::jmax(8, n / 10);
        for (int i = 0; i < n; ++i)
        {
            float& sample = frame[(size_t)i];
            sample = (float)((double)sample - mean);
            float window = 1.0f;
            if (i < taperSamples)
                window = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi
                                                * (float)i / (float)taperSamples);
            else if (i >= n - taperSamples)
                window = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi
                                                * (float)(n - 1 - i) / (float)taperSamples);
            sample *= window;
            energy += (double)sample * (double)sample;
        }
        const float frameRms = (float) std::sqrt(energy / (double) n);
        if (frameRms < rmsFloor)
            return false;

        const int minLag = juce::jmax(2, (int) std::floor(effectiveRate / 1000.0f));
        const int maxLag = juce::jmin(lagStorage - 2,
                                      juce::jmin(n / 2 - 2,
                                                 (int) std::floor(effectiveRate / minimumFrequency)));
        if (maxLag <= minLag + 2)
            return false;

        std::array<float, lagStorage> difference {};
        std::array<float, lagStorage> cmndf {};
        for (int lag = 1; lag <= maxLag; ++lag)
        {
            double sum = 0.0;
            for (int i = 0; i < n - lag; i += 2)
            {
                const float delta = frame[(size_t) i] - frame[(size_t) (i + lag)];
                sum += (double) delta * (double) delta;
            }
            difference[(size_t) lag] = (float) sum;
        }

        double cumulative = 0.0;
        cmndf[0] = 1.0f;
        for (int lag = 1; lag <= maxLag; ++lag)
        {
            cumulative += difference[(size_t) lag];
            cmndf[(size_t) lag] = cumulative > 1.0e-15
                ? (float) ((double) difference[(size_t) lag] * (double) lag / cumulative)
                : 1.0f;
        }

        int bestLag = 0;
        float bestValue = 1.0f;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            const float value = cmndf[(size_t) lag];
            if (value < bestValue)
            {
                bestValue = value;
                bestLag = lag;
            }
            if (value < yinThreshold)
            {
                // Find the bottom of the first credible trough.
                while (lag < maxLag && cmndf[(size_t) (lag + 1)] < cmndf[(size_t) lag])
                    ++lag;
                bestLag = lag;
                bestValue = cmndf[(size_t) lag];
                break;
            }
        }

        const float confidence = juce::jlimit(0.0f, 1.0f, 1.0f - bestValue);
        if (bestLag <= 0 || confidence < confidenceFloor)
            return false;
        if (rejectEdgeMinimum && bestLag >= maxLag - 5)
            return false; // short frame may be reporting a low note's upper harmonic

        float fractionalLag = (float) bestLag;
        if (bestLag > 1 && bestLag < maxLag)
        {
            const float left = cmndf[(size_t) (bestLag - 1)];
            const float centre = cmndf[(size_t) bestLag];
            const float right = cmndf[(size_t) (bestLag + 1)];
            const float denominator = left - 2.0f * centre + right;
            if (std::abs(denominator) > 1.0e-8f)
                fractionalLag += juce::jlimit(-0.5f, 0.5f, 0.5f * (left - right) / denominator);
        }

        const float estimatedHz = effectiveRate / juce::jmax(1.0f, fractionalLag);
        if (! std::isfinite(estimatedHz) || estimatedHz < minimumFrequency || estimatedHz > 1000.0f)
            return false;

        hzOut = estimatedHz;
        confidenceOut = confidence;
        return true;
    };

    float estimateHz = 0.0f;
    float confidence = 0.0f;
    bool voiced = false;

    if (lowLatency && pitchHistorySamples >= shortAnalysisWindowSamples)
        voiced = analyseWindow(shortAnalysisWindowSamples, shortAnalysisDecimation,
                               (float) currentSampleRate / (float) shortAnalysisDecimation,
                               90.0f, true, estimateHz, confidence);

    // Mid frame catches lower-tenor/alto notes without immediately paying the
    // latency of the long bass frame. It is only evaluated if the short frame
    // cannot find a confident fundamental.
    // Longer fallbacks are deliberately staggered: running all three YIN windows
    // on every 4 ms hop during silence is expensive and cannot improve the result.
    // Medium fallback runs every 2nd hop; the bass window runs every 4th hop.
    if (!voiced && (fallbackAnalysisCounter & 1) == 0
        && pitchHistorySamples >= mediumAnalysisWindowSamples)
        voiced = analyseWindow(mediumAnalysisWindowSamples, mediumAnalysisDecimation,
                               (float) currentSampleRate / (float) mediumAnalysisDecimation,
                               70.0f, true, estimateHz, confidence);

    if (!voiced && (fallbackAnalysisCounter & 3) == 0
        && pitchHistorySamples >= longAnalysisWindowSamples)
        voiced = analyseWindow(longAnalysisWindowSamples, longAnalysisDecimation,
                               (float) currentSampleRate / (float) longAnalysisDecimation,
                               55.0f, false, estimateHz, confidence);

    if (!voiced)
    {
        rejectFrame();
        return;
    }

    consecutiveUnvoicedAnalyses = 0;
    const float referenceHz = getPitchReferenceHz();
    float midi = 69.0f + 12.0f * std::log2(estimateHz / juce::jmax(1.0f, referenceHz));

    // YIN can occasionally lock to a strong second harmonic for a single frame.
    // If a confident tracked note suddenly moves almost exactly one octave, require
    // one consecutive confirming frame. A genuine octave jump adds only one analysis
    // hop; isolated octave slips are discarded without disturbing the current target.
    const float previousMidi = detectedMidiNote.load(std::memory_order_relaxed);
    const float previousConfidence = pitchConfidence.load(std::memory_order_relaxed);
    const float jump = midi - previousMidi;
    const bool octaveSizedJump = detectedHz.load(std::memory_order_relaxed) > 0.0f
        && previousConfidence >= 0.62f
        && std::abs(std::abs(jump) - 12.0f) <= 1.35f;
    if (octaveSizedJump)
    {
        if (pendingOctaveJumpConfirmations > 0
            && std::abs(midi - pendingOctaveJumpMidi) <= 0.85f)
        {
            ++pendingOctaveJumpConfirmations;
        }
        else
        {
            pendingOctaveJumpMidi = midi;
            pendingOctaveJumpConfirmations = 1;
        }

        if (pendingOctaveJumpConfirmations < 2)
        {
            // Keep the prior confidence with the prior held pitch for this one hop;
            // decaying below the gate threshold here would accidentally disable the
            // confirmation precisely on the next frame.
            return;
        }
    }
    pendingOctaveJumpConfirmations = 0;
    detectedHz.store(estimateHz, std::memory_order_relaxed);
    detectedMidiNote.store(midi, std::memory_order_relaxed);
    pitchConfidence.store(confidence, std::memory_order_relaxed);
}

void MetaTuneInspiredAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // The Low Latency switch controls both the preferred detector window and its
    // update cadence: 4 ms when enabled, 8 ms in the longer-window quality mode.
    // This reduces repeated YIN work in quality mode while keeping fast response
    // in the mode explicitly designed for low latency.
    const bool lowLatencyMode = lowLatencyParameter != nullptr
        && lowLatencyParameter->load(std::memory_order_relaxed) > 0.5f;
    if (lowLatencyMode != lastAnalysisLowLatencyMode)
    {
        lastAnalysisLowLatencyMode = lowLatencyMode;
        samplesSincePitchAnalysis = 0;
    }
    pitchAnalysisHopSamples = juce::jmax(64, juce::roundToInt((float)currentSampleRate
                                      * (lowLatencyMode ? 0.004f : 0.008f)));
    updatePitchEstimate(buffer);

    const float hz = detectedHz.load(std::memory_order_relaxed);
    const float confidence = pitchConfidence.load(std::memory_order_relaxed);
    // Audio-side hysteresis is deliberately lower than the UI's visual lock threshold.
    // The display remains confidence-gated at 0.60, but the shifter releases over ~1-2 hops.
    const bool voiced = hz > 0.0f && confidence >= 0.48f;
    const bool bypassed = globalBypassParameter != nullptr
        && globalBypassParameter->load(std::memory_order_relaxed) > 0.5f;

    if (!voiced)
    {
        heldPitchSamples = 0;
        hasHeldPitchAnchor = false;
        hasPendingScaleNote = false;
        pendingScaleSamples = 0;
        hasSmoothedTarget = false;
        // Do not zero the actual pitch shift here. processPitchShifter releases its
        // current correction smoothly when confidence drops, avoiding a block-edge snap.
        targetMidiNote.store(detectedMidiNote.load(std::memory_order_relaxed), std::memory_order_relaxed);
        // Keep the same fixed audio path/latency while unvoiced. With no reliable
        // pitch estimate the shifter runs at unity ratio and the Doubler can still run.
        processPitchShifter(buffer, 0.0f);
        // Keep the Doubler delay state warm even while globally bypassed so
        // re-engaging it never resumes from a stale delay buffer.
        processDoubler(buffer);
        processReferenceTone(buffer);
        return;
    }

    const float note = detectedMidiNote.load(std::memory_order_relaxed);
    const int scale = scaleParameter != nullptr ? (int)scaleParameter->load(std::memory_order_relaxed) : 0;
    const int root = rootParameter != nullptr ? (int)rootParameter->load(std::memory_order_relaxed) : 0;
    const float amount = amountParameter != nullptr
        ? amountParameter->load(std::memory_order_relaxed) / 100.0f : 1.0f;
    const float speedControl = speedParameter != nullptr
        ? speedParameter->load(std::memory_order_relaxed) : 30.0f;
    const float baseRetuneTimeMs = speedControlToMilliseconds(speedControl);
    const float sustain = sustainParameter != nullptr
        ? sustainParameter->load(std::memory_order_relaxed) : 0.0f;
    const bool stabilizerEnabled = stabilizerParameter != nullptr
        && stabilizerParameter->load(std::memory_order_relaxed) > 0.5f;
    const int stabilizerMode = stabilizerTimeParameter != nullptr
        ? (int)std::lround(stabilizerTimeParameter->load(std::memory_order_relaxed)) : 1;

    // A held-note tracker distinguishes sustained vowels from rapid pitch transitions.
    if (!hasHeldPitchAnchor)
    {
        heldPitchAnchorMidi = note;
        heldPitchSamples = 0;
        hasHeldPitchAnchor = true;
    }
    else if (std::abs(note - heldPitchAnchorMidi) <= 0.60f)
    {
        heldPitchSamples += buffer.getNumSamples();
        heldPitchAnchorMidi = heldPitchAnchorMidi * 0.88f + note * 0.12f;
    }
    else
    {
        heldPitchAnchorMidi = note;
        heldPitchSamples = 0;
    }
    const bool heldNote = heldPitchSamples >= (std::int64_t)(currentSampleRate * 0.14);

    static constexpr int major[] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int minor[] = { 0, 2, 3, 5, 7, 8, 10 };
    int nearest = (int)std::round(note);
    float bestDistance = 1000.0f;
    if (scale != 0)
    {
        for (int candidate = (int)std::floor(note) - 12;
             candidate <= (int)std::ceil(note) + 12; ++candidate)
        {
            const int pc = ((candidate - root) % 12 + 12) % 12;
            bool allowed = false;
            for (int j = 0; j < 7; ++j)
                allowed |= pc == (scale == 1 ? major[j] : minor[j]);
            if (allowed && std::abs(candidate - note) < bestDistance)
            {
                bestDistance = std::abs(candidate - note);
                nearest = candidate;
            }
        }
    }

    // Note Stabilizer rejects brief fragments before committing to a new scale note.
    static constexpr int stabilizerHoldMs[] = { 0, 40, 100, 200 };
    const int mode = juce::jlimit(0, 3, stabilizerMode);
    const int holdSamples = (int)(currentSampleRate * (float)stabilizerHoldMs[mode] / 1000.0f);
    if (!stabilizerEnabled || holdSamples <= 0)
    {
        committedScaleNote = nearest;
        hasCommittedScaleNote = true;
        hasPendingScaleNote = false;
        pendingScaleSamples = 0;
    }
    else if (!hasCommittedScaleNote)
    {
        committedScaleNote = nearest;
        hasCommittedScaleNote = true;
        hasPendingScaleNote = false;
        pendingScaleSamples = 0;
    }
    else if (nearest != committedScaleNote)
    {
        if (!hasPendingScaleNote || pendingScaleNote != nearest)
        {
            pendingScaleNote = nearest;
            pendingScaleSamples = 0;
            hasPendingScaleNote = true;
        }
        pendingScaleSamples += buffer.getNumSamples();
        if (pendingScaleSamples >= holdSamples)
        {
            committedScaleNote = pendingScaleNote;
            hasPendingScaleNote = false;
            pendingScaleSamples = 0;
        }
        else
        {
            nearest = committedScaleNote;
        }
    }
    else
    {
        hasPendingScaleNote = false;
        pendingScaleSamples = 0;
    }

    const float target = note + ((float)nearest - note) * amount;
    const float requestedCorrectionSemitones = target - note;
    targetMidiNote.store(target, std::memory_order_relaxed);

    // Negative Speed values form the immediate hard-tune region. Sustain changes
    // response only after the held-note tracker recognizes a stable vowel.
    float effectiveMs = baseRetuneTimeMs;
    if (baseRetuneTimeMs < 0.0f)
        effectiveMs = 0.0f;
    else if (heldNote)
    {
        const float sustainFactor = sustain >= 0.0f
            ? (1.0f - 0.70f * sustain / 100.0f)
            : (1.0f + 1.50f * (-sustain) / 100.0f);
        effectiveMs *= juce::jmax(0.10f, sustainFactor);
    }
    effectiveRetuneTimeMs.store(effectiveMs, std::memory_order_relaxed);

    if (!hasSmoothedTarget)
    {
        // Initialise the display target to the detected note. The live correction
        // retains its own sample-smoothed state and is released by the shifter.
        audioSmoothedTargetMidi = note;
        hasSmoothedTarget = true;
    }
    const float alpha = effectiveMs <= 0.05f ? 1.0f
        : 1.0f - std::exp(-(float)buffer.getNumSamples()
                          / (float)(currentSampleRate * (effectiveMs / 1000.0f)));
    const float smoothing = juce::jlimit(0.0f, 1.0f, alpha);
    audioSmoothedTargetMidi += (target - audioSmoothedTargetMidi) * smoothing;
    // Keep the displayed target smooth, but slew the actual correction sample-by-sample
    // inside the shifter so response is independent of the host's block size.
    smoothedTargetMidiNote.store(audioSmoothedTargetMidi, std::memory_order_relaxed);

    processPitchShifter(buffer, bypassed ? 0.0f : requestedCorrectionSemitones);
    // processDoubler zeroes its wet controls in global bypass but keeps its delay
    // line advancing, avoiding stale-buffer clicks on reactivation.
    processDoubler(buffer);
    processReferenceTone(buffer);
}

void MetaTuneInspiredAudioProcessor::processReferenceTone(juce::AudioBuffer<float>& buffer) noexcept
{
    const int channels = juce::jmin(2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    const int note = tonePreviewMidiNote.load(std::memory_order_relaxed);
    const float referenceHz = pitchReferenceParameter != nullptr
        ? pitchReferenceParameter->load(std::memory_order_relaxed) : 440.0f;
    const float targetFrequency = note >= 0
        ? juce::jlimit(20.0f, 2000.0f, referenceHz * std::exp2(((float)note - 69.0f) / 12.0f))
        : referenceToneFrequencyHz;
    const float volume = toneVolumeParameter != nullptr
        ? juce::jlimit(0.0f, 100.0f, toneVolumeParameter->load(std::memory_order_relaxed)) / 100.0f
        : 0.65f;
    // Key-up must actually release the tone. Keep phase moving during release and
    // slew pitch when moving between piano keys to avoid a waveform discontinuity.
    const float targetAmplitude = note >= 0 ? 0.16f * volume : 0.0f;
    const float attackAlpha = 1.0f - std::exp(-1.0f / (float)(currentSampleRate * 0.004));
    const float releaseAlpha = 1.0f - std::exp(-1.0f / (float)(currentSampleRate * 0.028));
    const float frequencyAlpha = 1.0f - std::exp(-1.0f / (float)(currentSampleRate * 0.008));
    std::array<float*, 2> outputPointers { buffer.getWritePointer(0), nullptr };
    if (channels > 1)
        outputPointers[1] = buffer.getWritePointer(1);
    for (int i = 0; i < samples; ++i)
    {
        if (note >= 0)
            referenceToneFrequencyHz += (targetFrequency - referenceToneFrequencyHz) * frequencyAlpha;
        const float alpha = targetAmplitude > referenceToneAmplitude ? attackAlpha : releaseAlpha;
        referenceToneAmplitude += (targetAmplitude - referenceToneAmplitude) * alpha;
        if (referenceToneAmplitude < 1.0e-6f && targetAmplitude == 0.0f)
            referenceToneAmplitude = 0.0f;
        if (referenceToneAmplitude > 1.0e-6f)
        {
            const float tone = std::sin(referenceTonePhase) * referenceToneAmplitude;
            for (int channel = 0; channel < channels; ++channel)
                outputPointers[(size_t)channel][i] += tone;
        }
        referenceTonePhase += juce::MathConstants<float>::twoPi * referenceToneFrequencyHz
                            / (float)currentSampleRate;
        if (referenceTonePhase >= juce::MathConstants<float>::twoPi)
            referenceTonePhase -= juce::MathConstants<float>::twoPi;
    }
}

void MetaTuneInspiredAudioProcessor::processPitchShifter(juce::AudioBuffer<float>& buffer,
                                                          float semitoneShift) noexcept
{
    const int channels = juce::jmin(2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    const float retuneMs = effectiveRetuneTimeMs.load(std::memory_order_relaxed);
    const bool targetingUnity = std::abs(semitoneShift) <= 0.004f;
    // A hard-tune request uses a sub-ms slew; natural settings map to their actual
    // millisecond response (95% convergence in roughly the displayed retune time).
    // Releases to unity use a separate 14 ms envelope so unvoiced frames do not click.
    const float correctionSmoothingMs = targetingUnity
        ? (std::abs(audioSmoothedCorrectionSemitones) > 0.004f ? 14.0f : 0.25f)
        : (retuneMs <= 0.05f ? 0.25f : juce::jlimit(0.15f, 66.7f, retuneMs / 3.0f));
    const float correctionAlpha = 1.0f - std::exp(-1.0f
        / (float)(currentSampleRate * (correctionSmoothingMs * 0.001f)));
    const float activationAlpha = 1.0f - std::exp(-1.0f / (float)(currentSampleRate * 0.006));
    const float fixedDelay = (float)pitchShiftBaseDelaySamples + (float)pitchShiftWindowSamples * 0.5f;
    constexpr float unityEpsilon = 0.00025f;
    constexpr float pi = juce::MathConstants<float>::pi;

    // Four-point Catmull-Rom interpolation is smoother at fractional delays than
    // linear interpolation, with no allocation or external DSP dependency.
    auto readDelay = [this](int channel, float delaySamples) noexcept -> float
    {
        float readPosition = (float)pitchShiftWritePosition - delaySamples;
        if (readPosition < 0.0f) readPosition += (float)pitchShiftBufferSize;
        if (readPosition >= (float)pitchShiftBufferSize) readPosition -= (float)pitchShiftBufferSize;
        const int i1 = (int)readPosition;
        const int i0 = (i1 - 1 + pitchShiftBufferSize) % pitchShiftBufferSize;
        const int i2 = (i1 + 1) % pitchShiftBufferSize;
        const int i3 = (i1 + 2) % pitchShiftBufferSize;
        const float t = readPosition - (float)i1;
        const auto& line = pitchShiftDelayLine[(size_t)channel];
        const float p0 = line[(size_t)i0], p1 = line[(size_t)i1];
        const float p2 = line[(size_t)i2], p3 = line[(size_t)i3];
        const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
        const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const float c = -0.5f * p0 + 0.5f * p2;
        return ((a * t + b) * t + c) * t + p1;
    };

    const float targetWet = std::abs(semitoneShift) > 0.004f ? 1.0f : 0.0f;
    std::array<float*, 2> outputPointers { buffer.getWritePointer(0), nullptr };
    if (channels > 1)
        outputPointers[1] = buffer.getWritePointer(1);
    for (int i = 0; i < samples; ++i)
    {
        audioSmoothedCorrectionSemitones +=
            (semitoneShift - audioSmoothedCorrectionSemitones) * correctionAlpha;
        currentPitchRatio = juce::jlimit(0.5f, 2.0f,
            std::exp2(audioSmoothedCorrectionSemitones / 12.0f));
        if (std::abs(currentPitchRatio - 1.0f) < unityEpsilon)
        {
            currentPitchRatio = 1.0f;
            if (targetingUnity && std::abs(audioSmoothedCorrectionSemitones) < 0.004f)
                audioSmoothedCorrectionSemitones = 0.0f;
        }
        pitchShiftWet += (targetWet - pitchShiftWet) * activationAlpha;
        pitchShiftWet = juce::jlimit(0.0f, 1.0f, pitchShiftWet);

        float phaseA = pitchShiftPhase;
        float phaseB = phaseA + 0.5f;
        if (phaseB >= 1.0f) phaseB -= 1.0f;
        const float sinA = std::sin(pi * phaseA);
        const float weightA = sinA * sinA;
        const float weightB = 1.0f - weightA;
        const float delayA = (float)pitchShiftBaseDelaySamples
                           + phaseA * (float)pitchShiftWindowSamples;
        const float delayB = (float)pitchShiftBaseDelaySamples
                           + phaseB * (float)pitchShiftWindowSamples;

        for (int channel = 0; channel < channels; ++channel)
        {
            auto* data = outputPointers[(size_t)channel];
            pitchShiftDelayLine[(size_t)channel][(size_t)pitchShiftWritePosition] = data[i];

            const float alignedDry = readDelay(channel, fixedDelay);
            const float headA = readDelay(channel, delayA);
            const float headB = readDelay(channel, delayB);
            const float shifted = headA * weightA + headB * weightB;
            // Fade between a latency-matched unity path and the two-head shifter so
            // Amount=0/unvoiced changes do not abruptly switch timbre or click.
            data[i] = alignedDry + (shifted - alignedDry) * pitchShiftWet;
        }

        if (currentPitchRatio == 1.0f)
            pitchShiftPhase = 0.25f;
        else
        {
            // Variable delay slope D' = 1 - ratio gives the desired pitch ratio.
            pitchShiftPhase += (1.0f - currentPitchRatio) / (float)pitchShiftWindowSamples;
            pitchShiftPhase -= std::floor(pitchShiftPhase);
        }
        pitchShiftWritePosition = (pitchShiftWritePosition + 1) % pitchShiftBufferSize;
    }
    correctionCents.store(audioSmoothedCorrectionSemitones * 100.0f, std::memory_order_relaxed);
}

void MetaTuneInspiredAudioProcessor::processDoubler(juce::AudioBuffer<float>& buffer) noexcept
{
    const int channels = juce::jmin(2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    const bool globallyBypassed = globalBypassParameter != nullptr
        && globalBypassParameter->load(std::memory_order_relaxed) > 0.5f;
    const float targetWidth = !globallyBypassed && doublerWidthParameter != nullptr
        ? juce::jlimit(0.0f, 1.0f, doublerWidthParameter->load(std::memory_order_relaxed) / 100.0f)
        : (globallyBypassed ? 0.0f : 0.46f);
    const float targetMix = !globallyBypassed && doublerMixParameter != nullptr
        ? juce::jlimit(0.0f, 1.0f, doublerMixParameter->load(std::memory_order_relaxed) / 100.0f)
        : (globallyBypassed ? 0.0f : 0.49f);
    const float parameterSmoothing = 1.0f - std::exp(-1.0f / (float)(currentSampleRate * 0.008));
    const float centreDelay = (float)currentSampleRate * 0.014f;
    const float lfoIncrement = juce::MathConstants<float>::twoPi * 0.21f / (float)currentSampleRate;
    auto* left = buffer.getWritePointer(0);
    auto* right = channels > 1 ? buffer.getWritePointer(1) : nullptr;
    auto readDelay = [this](int channel, float delaySamples) noexcept -> float
    {
        float readPosition = (float)doublerWritePosition - delaySamples;
        if (readPosition < 0.0f) readPosition += (float)pitchShiftBufferSize;
        if (readPosition >= (float)pitchShiftBufferSize) readPosition -= (float)pitchShiftBufferSize;
        const int i1 = (int)readPosition;
        const int i0 = (i1 - 1 + pitchShiftBufferSize) % pitchShiftBufferSize;
        const int i2 = (i1 + 1) % pitchShiftBufferSize;
        const int i3 = (i1 + 2) % pitchShiftBufferSize;
        const float t = readPosition - (float)i1;
        const auto& line = doublerDelayLine[(size_t)channel];
        const float p0 = line[(size_t)i0], p1 = line[(size_t)i1];
        const float p2 = line[(size_t)i2], p3 = line[(size_t)i3];
        const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
        const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const float c = -0.5f * p0 + 0.5f * p2;
        return ((a * t + b) * t + c) * t + p1;
    };

    for (int i = 0; i < samples; ++i)
    {
        // Smooth macro movement and bypass transitions sample-by-sample to avoid
        // zipper noise when dragging the Doubler dot during playback.
        smoothedDoublerWidth += (targetWidth - smoothedDoublerWidth) * parameterSmoothing;
        smoothedDoublerMix += (targetMix - smoothedDoublerMix) * parameterSmoothing;
        const float width = smoothedDoublerWidth;
        const float mix = smoothedDoublerMix;
        const float depth = (float)currentSampleRate * (0.0005f + 0.0025f * width);
        const float inL = left[i];
        const float inR = channels > 1 ? right[i] : inL;
        doublerDelayLine[0][(size_t)doublerWritePosition] = inL;
        if (channels > 1)
            doublerDelayLine[1][(size_t)doublerWritePosition] = inR;

        const float lfo = std::sin(doublerLfoPhase);
        const float delayL = centreDelay + lfo * depth;
        const float delayR = centreDelay - lfo * depth;
        const float delayedL = readDelay(0, delayL);
        const float delayedR = channels > 1 ? readDelay(1, delayR) : readDelay(0, delayR);

        // Width acts in the mid/side domain; wet mix blends in decorrelated, gently
        // modulated voices rather than merely drawing the two Doubler rings.
        const float mid = 0.5f * (inL + inR);
        const float side = 0.5f * (inL - inR) * (1.0f + 0.75f * width * mix);
        const float wideL = mid + side;
        const float wideR = mid - side;
        constexpr float doubledVoiceBlend = 0.45f;
        const float wetL = wideL * (1.0f - doubledVoiceBlend) + delayedL * doubledVoiceBlend;
        const float wetR = wideR * (1.0f - doubledVoiceBlend) + delayedR * doubledVoiceBlend;
        left[i] = wideL * (1.0f - mix) + wetL * mix;
        if (channels > 1)
            right[i] = wideR * (1.0f - mix) + wetR * mix;

        doublerLfoPhase += lfoIncrement;
        if (doublerLfoPhase >= juce::MathConstants<float>::twoPi)
            doublerLfoPhase -= juce::MathConstants<float>::twoPi;
        doublerWritePosition = (doublerWritePosition + 1) % pitchShiftBufferSize;
    }
}

void MetaTuneInspiredAudioProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, dest);
}

void MetaTuneInspiredAudioProcessor::setStateInformation(const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, size));
    if (xml && xml->hasTagName(parameters.state.getType()))
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* MetaTuneInspiredAudioProcessor::createEditor()
{
    return new MetaTuneInspiredAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MetaTuneInspiredAudioProcessor();
}
