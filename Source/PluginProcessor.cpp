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
    return p;
}

void MetaTuneInspiredAudioProcessor::prepareToPlay(double sr, int maxBlock)
{
    juce::ignoreUnused(maxBlock);
    currentSampleRate = sr;
    // Keep analysis windows approximately constant in milliseconds across common
    // sample rates. The low-latency window is about 21-23 ms; the bass fallback
    // is about 42-46 ms and supports lower fundamentals without alias-prone lags.
    shortAnalysisDecimation = juce::jmax(1, juce::roundToInt((float)sr / 48000.0f));
    shortAnalysisWindowSamples = 1024 * shortAnalysisDecimation;
    longAnalysisDecimation = juce::jmax(2, juce::roundToInt((float)sr / 24000.0f));
    longAnalysisWindowSamples = 1024 * longAnalysisDecimation;
    pitchAnalysisHopSamples = juce::jmax(256, juce::roundToInt((float)sr * 0.005f));
    pitchHistory.fill(0.0f);
    pitchHistoryWritePosition = 0;
    pitchHistorySamples = 0;
    samplesSincePitchAnalysis = 0;
    consecutiveUnvoicedAnalyses = 0;
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
    // Feed a fixed-size mono ring buffer from every callback, including small
    // 32/64/128/256-sample ASIO/FL Studio blocks. No allocation occurs here.
    const int channels = buffer.getNumChannels();
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    double blockEnergy = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        float mono = 0.0f;
        for (int c = 0; c < channels; ++c)
            mono += buffer.getReadPointer(c)[i];
        mono /= (float) channels;
        blockEnergy += (double) mono * (double) mono;

        pitchHistory[(size_t) pitchHistoryWritePosition] = mono;
        pitchHistoryWritePosition = (pitchHistoryWritePosition + 1) % pitchHistorySize;
        pitchHistorySamples = juce::jmin(pitchHistorySize, pitchHistorySamples + 1);
        ++samplesSincePitchAnalysis;

        // 256 samples at a time keeps tracking responsive without a full YIN
        // computation on every tiny host callback.
        if (samplesSincePitchAnalysis >= pitchAnalysisHopSamples)
        {
            samplesSincePitchAnalysis -= pitchAnalysisHopSamples;
            estimatePitchFromHistory();
        }
    }

    inputRms.store((float) std::sqrt(blockEnergy / (double) samples), std::memory_order_relaxed);
}

void MetaTuneInspiredAudioProcessor::estimatePitchFromHistory() noexcept
{
    constexpr int frameSize = 1024;
    constexpr int maxLagStorage = 512;
    constexpr float minimumRms = 0.0025f;
    constexpr float minimumConfidence = 0.60f;

    if (pitchHistorySamples < frameSize || currentSampleRate < 8000.0)
        return;

    auto rejectFrame = [this]() noexcept
    {
        pitchConfidence.store(0.0f, std::memory_order_relaxed);
        if (++consecutiveUnvoicedAnalyses >= 3)
        {
            detectedHz.store(0.0f, std::memory_order_relaxed);
            correctionCents.store(0.0f, std::memory_order_relaxed);
        }
    };

    // The short frame is preferred in Low Latency mode. It detects notes roughly
    // above 90 Hz using only the most recent 1024 samples. Notes too low for that
    // frame fall back to a longer decimated frame, avoiding octave guesses at the
    // upper lag limit. This is analysis look-back, not an added audio-buffer delay.
    const auto analyseWindow = [this](int sourceSamples, int decimation,
                                      float effectiveRate, float minimumFrequency,
                                      bool rejectEdgeMinimum,
                                      float& hzOut, float& confidenceOut) noexcept -> bool
    {
        constexpr int n = 1024;
        constexpr int lagStorage = 512;
        constexpr float rmsFloor = 0.0025f;
        constexpr float yinThreshold = 0.18f;
        constexpr float confidenceFloor = 0.60f;
        if (pitchHistorySamples < sourceSamples)
            return false;

        std::array<float, n> frame {};
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
        for (auto& sample : frame)
        {
            sample = (float) ((double) sample - mean);
            energy += (double) sample * (double) sample;
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

    const bool lowLatency = lowLatencyParameter != nullptr
        && lowLatencyParameter->load(std::memory_order_relaxed) > 0.5f;
    float estimateHz = 0.0f;
    float confidence = 0.0f;
    bool voiced = false;

    if (lowLatency && pitchHistorySamples >= shortAnalysisWindowSamples)
        voiced = analyseWindow(shortAnalysisWindowSamples, shortAnalysisDecimation,
                               (float) currentSampleRate / (float) shortAnalysisDecimation,
                               90.0f, true, estimateHz, confidence);

    if (!voiced && pitchHistorySamples >= longAnalysisWindowSamples)
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
    const float midi = 69.0f + 12.0f * std::log2(estimateHz / juce::jmax(1.0f, referenceHz));
    detectedHz.store(estimateHz, std::memory_order_relaxed);
    detectedMidiNote.store(midi, std::memory_order_relaxed);
    pitchConfidence.store(confidence, std::memory_order_relaxed);
}

void MetaTuneInspiredAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    updatePitchEstimate(buffer);

    const float hz = detectedHz.load(std::memory_order_relaxed);
    const float confidence = pitchConfidence.load(std::memory_order_relaxed);
    const bool voiced = hz > 0.0f && confidence >= 0.60f;
    if (!voiced)
    {
        heldPitchSamples = 0;
        hasHeldPitchAnchor = false;
        correctionCents.store(0.0f, std::memory_order_relaxed);
        return; // keep pass-through; don't chase noise or stale pitch estimates
    }

    const float note = detectedMidiNote.load(std::memory_order_relaxed);
    const int scale = scaleParameter != nullptr ? (int) scaleParameter->load(std::memory_order_relaxed) : 0;
    const int root = rootParameter != nullptr ? (int) rootParameter->load(std::memory_order_relaxed) : 0;
    const float amount = amountParameter != nullptr ? amountParameter->load(std::memory_order_relaxed) / 100.0f : 1.0f;
    const float speedControl = speedParameter != nullptr ? speedParameter->load(std::memory_order_relaxed) : 30.0f;
    const float baseRetuneTimeMs = speedControlToMilliseconds(speedControl);
    const float sustain = sustainParameter != nullptr ? sustainParameter->load(std::memory_order_relaxed) : 0.0f;
    const bool stabilizerEnabled = stabilizerParameter != nullptr
        && stabilizerParameter->load(std::memory_order_relaxed) > 0.5f;
    const int stabilizerMode = stabilizerTimeParameter != nullptr
        ? (int) std::lround(stabilizerTimeParameter->load(std::memory_order_relaxed)) : 1;

    // A light note-hold tracker separates a sustained vowel from a rapid pitch
    // transition. Vibrato within about 0.6 semitone counts as the same held note.
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
    const bool heldNote = heldPitchSamples >= (std::int64_t) (currentSampleRate * 0.14);

    static constexpr int major[] = {0,2,4,5,7,9,11};
    static constexpr int minor[] = {0,2,3,5,7,8,10};
    int nearest = (int) std::round(note);
    float bestDistance = 1000.0f;
    if (scale != 0)
    {
        for (int candidate = (int) std::floor(note) - 12;
             candidate <= (int) std::ceil(note) + 12; ++candidate)
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

    // Note Stabilizer independently ignores brief note fragments before committing
    // a new scale note. Low Latency changes the pitch-analysis frame above; both
    // options can remain active at the same time.
    static constexpr int stabilizerHoldMs[] = { 0, 40, 100, 200 };
    const int mode = juce::jlimit(0, 3, stabilizerMode);
    const int holdSamples = (int) (currentSampleRate * (float) stabilizerHoldMs[mode] / 1000.0f);
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

    const float target = note + ((float) nearest - note) * amount;
    correctionCents.store((target - note) * 100.0f, std::memory_order_relaxed);
    targetMidiNote.store(target, std::memory_order_relaxed);

    // Base Speed ranges from 200 ms to -3 ms. Negative values represent the
    // hard-tune region and therefore use an immediate target step. Sustain only
    // modifies smoothing after a stable note has been held long enough.
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

    // Smooth the target trajectory block-by-block with a sample-rate-aware
    // exponential coefficient. This is the control trajectory a future low-latency
    // PSOLA/granular shifter will follow; audio remains pass-through until that
    // shifter is implemented and validated against latency/artifact tests.
    if (!hasSmoothedTarget)
    {
        audioSmoothedTargetMidi = target;
        hasSmoothedTarget = true;
    }
    else
    {
        const float alpha = effectiveMs <= 0.05f ? 1.0f
            : 1.0f - std::exp(-(float) buffer.getNumSamples()
                              / (float) (currentSampleRate * (effectiveMs / 1000.0f)));
        audioSmoothedTargetMidi += (target - audioSmoothedTargetMidi) * juce::jlimit(0.0f, 1.0f, alpha);
    }
    smoothedTargetMidiNote.store(audioSmoothedTargetMidi, std::memory_order_relaxed);
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
