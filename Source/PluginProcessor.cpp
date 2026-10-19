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
    glideParameter = parameters.getRawParameterValue("glide");
    correctionLimitParameter = parameters.getRawParameterValue("correctionLimit");
    precisionHardTuneParameter = parameters.getRawParameterValue("precisionHardTune");
    noteConfirmParameter = parameters.getRawParameterValue("noteConfirm");
    consonantReleaseParameter = parameters.getRawParameterValue("consonantRelease");
    scaleParameter = parameters.getRawParameterValue("scale");
    for (int i = 0; i < 12; ++i)
        customDegreeParameters[(size_t)i] = parameters.getRawParameterValue("customDegree" + juce::String(i));
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
    p.add(std::make_unique<AudioParameterFloat>("glide", "Glide (ms)", NormalisableRange<float>(0.0f, 500.0f, 1.0f), 0.0f));
    p.add(std::make_unique<AudioParameterFloat>("correctionLimit", "Max Correction (semitones)", NormalisableRange<float>(0.0f, 12.0f, 0.1f), 12.0f));
    p.add(std::make_unique<AudioParameterBool>("precisionHardTune", "Precision Hard Tune", false));
    p.add(std::make_unique<AudioParameterFloat>("noteConfirm", "Note Confirmation (ms)", NormalisableRange<float>(0.0f, 100.0f, 1.0f), 10.0f));
    p.add(std::make_unique<AudioParameterFloat>("consonantRelease", "Consonant Release (ms)", NormalisableRange<float>(0.0f, 30.0f, 1.0f), 8.0f));
    p.add(std::make_unique<AudioParameterBool>("stabilizer", "Note Stabilizer", true));
    p.add(std::make_unique<AudioParameterChoice>("stabilizerTime", "Note Stabilizer Time",
                                                  StringArray { "None", "Short", "Mid", "Long" }, 1));
    p.add(std::make_unique<AudioParameterBool>("lowLatency", "Low Latency", true));
    p.add(std::make_unique<AudioParameterChoice>("scale", "Scale",
        StringArray{"Chromatic", "Major", "Minor", "Harmonic Minor", "Major Pentatonic",
                    "Minor Pentatonic", "Blues", "Dorian", "Custom"}, 0));
    // Degrees relative to the selected root, not absolute piano keys. These IDs
    // are additive, preserving the existing automation and session format.
    constexpr bool defaultDegrees[12] = {true, false, true, false, true, true,
                                          false, true, false, true, false, true};
    for (int i = 0; i < 12; ++i)
        p.add(std::make_unique<AudioParameterBool>("customDegree" + String(i),
            "Custom degree " + String(i), defaultDegrees[i]));
    p.add(std::make_unique<AudioParameterInt>("root", "Root Note", 0, 11, 4));
    p.add(std::make_unique<AudioParameterFloat>("doublerWidth", "Doubler Width",
        NormalisableRange<float>(0.0f, 100.0f, 0.1f), 46.0f));
    p.add(std::make_unique<AudioParameterFloat>("doublerMix", "Doubler Mix",
        NormalisableRange<float>(0.0f, 100.0f, 0.1f), 0.0f));
    p.add(std::make_unique<AudioParameterBool>("globalBypass", "Global Bypass", false));
    p.add(std::make_unique<AudioParameterFloat>("toneVolume", "Tone Volume",
        NormalisableRange<float>(0.0f, 100.0f, 0.1f), 65.0f));
    return p;
}

void MetaTuneInspiredAudioProcessor::prepareToPlay(double sr, int maxBlock)
{
    currentSampleRate = juce::jmax(8000.0, sr);

    // Pulse-aligned TD-PSOLA tracks 70-1000 Hz. The same fixed budget is both
    // reported to the host and enforced by the output compensation stage below.
    realtimeAutotune.prepare(currentSampleRate, 70.0f, 1000.0f);
    // Source-period repetition can push the measured normal-mode delay to about
    // 2.35 periods at the 70 Hz lower tracking limit. Reserve 2.5 periods so the
    // output compensation stage always has non-negative padding in that corner.
#if defined(SKELLTUNE_LIVE_FIXED_LOW_LATENCY) && SKELLTUNE_LIVE_FIXED_LOW_LATENCY
    // Standalone Live variant: fast grain scheduler cannot be disabled, so
    // a smaller host-reported PDC budget is safe across automation changes.
    fixedLatencySamples = autotune::RealtimeAutotune::recommendedHostLatencySamples(currentSampleRate, 70.0f, true);
#else
    fixedLatencySamples = autotune::RealtimeAutotune::recommendedHostLatencySamples(currentSampleRate, 70.0f);
#endif
    setLatencySamples(fixedLatencySamples);
    outputLatencyCompensator.prepare(fixedLatencySamples, currentSampleRate);
    outputLatencyCompensator.setPitchPreserving(true);
    referenceToneDelay.prepare(fixedLatencySamples);
    latencyAlignedBypass.prepare(fixedLatencySamples, currentSampleRate);
    // Channel-major scratch storage is bounded, allocated only in prepareToPlay.
    // processBlock divides arbitrary host blocks into slices that fit this buffer;
    // it NEVER silently discards intrinsic-latency telemetry for oversize blocks.
    intrinsicLatencyScratch.assign(2u * (size_t)juce::jmax(1, juce::jmin(maxBlock, 4096)), 0.0f);
    dryLatencyScratch.assign(intrinsicLatencyScratch.size(), 0.0f);

    // The Doubler delay lines are fixed-capacity and reset outside the audio callback.
    doublerDelayLine = {};
    doublerWritePosition = 0;
    referenceTonePhase = 0.0f;
    referenceToneAmplitude = 0.0f;
    referenceToneFrequencyHz = pitchReferenceParameter != nullptr
        ? pitchReferenceParameter->load(std::memory_order_relaxed) : 440.0f;
    tonePreviewMidiNote.store(-1, std::memory_order_relaxed);
    doublerLfoPhase = 0.0f;
    smoothedDoublerWidth = doublerWidthParameter != nullptr
        ? juce::jlimit(0.0f, 1.0f, doublerWidthParameter->load(std::memory_order_relaxed) / 100.0f) : 0.46f;
    smoothedDoublerMix = doublerMixParameter != nullptr
        ? juce::jlimit(0.0f, 1.0f, doublerMixParameter->load(std::memory_order_relaxed) / 100.0f) : 0.0f;
    detectedHz.store(0.0f, std::memory_order_relaxed);
    detectedMidiNote.store(69.0f, std::memory_order_relaxed);
    correctionCents.store(0.0f, std::memory_order_relaxed);
    pitchConfidence.store(0.0f, std::memory_order_relaxed);
    inputRms.store(0.0f, std::memory_order_relaxed);
    targetMidiNote.store(69.0f, std::memory_order_relaxed);
    smoothedTargetMidiNote.store(69.0f, std::memory_order_relaxed);
    effectiveRetuneTimeMs.store(30.0f, std::memory_order_relaxed);
}

void MetaTuneInspiredAudioProcessor::releaseResources() {}

bool MetaTuneInspiredAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void MetaTuneInspiredAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin(2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    const bool bypassed = globalBypassParameter != nullptr
        && globalBypassParameter->load(std::memory_order_relaxed) > 0.5f;
    const float amount = amountParameter != nullptr
        ? juce::jlimit(0.0f, 100.0f, amountParameter->load(std::memory_order_relaxed)) / 100.0f
        : 1.0f;
    const float speed = speedParameter != nullptr
        ? speedParameter->load(std::memory_order_relaxed) : 30.0f;
    const float sustain = sustainParameter != nullptr
        ? sustainParameter->load(std::memory_order_relaxed) : 0.0f;
    const int scaleChoice = scaleParameter != nullptr
        ? (int)std::lround(scaleParameter->load(std::memory_order_relaxed)) : 0;
    const int root = rootParameter != nullptr
        ? (int)std::lround(rootParameter->load(std::memory_order_relaxed)) : 0;
    const float referenceHz = pitchReferenceParameter != nullptr
        ? pitchReferenceParameter->load(std::memory_order_relaxed) : 440.0f;
    const bool stabilizer = stabilizerParameter != nullptr
        && stabilizerParameter->load(std::memory_order_relaxed) > 0.5f;

    autotune::Settings settings;
    settings.key = juce::jlimit(0, 11, root);
    settings.scale = static_cast<autotune::Scale>(juce::jlimit(0, (int)autotune::Scale::Custom, scaleChoice));
    for (int i = 0; i < 12; ++i)
        settings.customScale[(size_t)i] = customDegreeParameters[(size_t)i] != nullptr
            && customDegreeParameters[(size_t)i]->load(std::memory_order_relaxed) > 0.5f;
    settings.referenceHz = juce::jlimit(415.3047f, 466.1638f, referenceHz);
    const float mappedMs = speedControlToMilliseconds(speed);
    settings.retuneMs = mappedMs <= 0.0f ? 0.0f : mappedMs;
    // Sustain lengthens or shortens retuning in the same direction as the existing UI macro.
    if (settings.retuneMs > 0.0f)
    {
        const float sustainFactor = sustain >= 0.0f
            ? (1.0f - 0.70f * juce::jlimit(0.0f, 100.0f, sustain) / 100.0f)
            : (1.0f + 1.50f * juce::jlimit(0.0f, 100.0f, -sustain) / 100.0f);
        settings.retuneMs *= juce::jmax(0.10f, sustainFactor);
    }
    settings.glideMs = glideParameter != nullptr
        ? glideParameter->load(std::memory_order_relaxed) : 0.0f;
    settings.maxCorrectionSemitones = correctionLimitParameter != nullptr
        ? correctionLimitParameter->load(std::memory_order_relaxed) : 12.0f;
    settings.noteConfirmMs = noteConfirmParameter != nullptr
        ? noteConfirmParameter->load(std::memory_order_relaxed) : 10.0f;
    settings.consonantReleaseMs = consonantReleaseParameter != nullptr
        ? consonantReleaseParameter->load(std::memory_order_relaxed) : 8.0f;
    settings.precisionHardTune = precisionHardTuneParameter != nullptr
        && precisionHardTuneParameter->load(std::memory_order_relaxed) > 0.5f;
    settings.strength = amount;
    settings.humanize = 0.0f;
    settings.vibratoPreserve = 0.70f;
    settings.follow = 0.85f; // responsive for rap/spoken vocals while retaining some centre stability
    settings.voicingThreshold = stabilizer ? 0.85f : 0.68f;
#if defined(SKELLTUNE_LIVE_FIXED_LOW_LATENCY) && SKELLTUNE_LIVE_FIXED_LOW_LATENCY
    // Do not let an automated normal-mode toggle exceed the smaller Live PDC.
    settings.lowLatency = true;
#else
    settings.lowLatency = lowLatencyParameter != nullptr
        && lowLatencyParameter->load(std::memory_order_relaxed) > 0.5f;
#endif
    // None disables the slow pitch centre; progressively longer settings reject
    // vibrato-driven target changes. The default Short retains the prior 300 ms.
    const int stabilizerTimeChoice = stabilizerTimeParameter != nullptr
        ? (int) std::lround (stabilizerTimeParameter->load(std::memory_order_relaxed)) : 1;
    constexpr std::array<float, 4> stabilizerTimesMs { 0.0f, 300.0f, 450.0f, 650.0f };
    settings.noteStabilizerMs = stabilizer
        ? stabilizerTimesMs[(size_t)juce::jlimit (0, 3, stabilizerTimeChoice)] : 0.0f;
    // Keep the DSP history warm during host bypass. The final bypass crossfade
    // happens after host PDC, between time-aligned paths, not with raw input.
    settings.bypass = false;
    realtimeAutotune.setSettings(settings); // already on the audio thread; no lock or cross-thread hand-off needed

    // Input metering is pre-tune. The engine follows the stronger channel for pitch
    // analysis and runs independent, pitch-linked PSOLA streams to preserve stereo.
    double energy = 0.0;
    for (int c = 0; c < channels; ++c)
    {
        const float* d = buffer.getReadPointer(c);
        for (int i = 0; i < samples; ++i)
            energy += (double)d[i] * (double)d[i];
    }
    inputRms.store((float)std::sqrt(energy / (double)(channels * samples)), std::memory_order_relaxed);

    // Channel-major latency telemetry uses the length of EACH slice as its stride.
    // Process the ENTIRE chain (tuning, doubler, reference tone and output pad)
    // in order for each slice, so even hosts sending unexpectedly huge blocks
    // retain the same fixed-PDC path. No allocations or resize on the audio thread.
    const int sliceCapacity = (int) (intrinsicLatencyScratch.size() / 2u);
    if (sliceCapacity <= 0)
    {
        // Defensive guard for an invalid/unprepared instance. JUCE normally
        // guarantees prepareToPlay first. Do not run tuning without a matching
        // delay timeline if that contract is violated.
        buffer.clear();
        return;
    }

    latencyAlignedBypass.setBypassed(bypassed);
    // While bypassed, retain the established host-aligned dry timeline.
    // The voiced path uses stable padding to avoid undoing pitch correction.
    outputLatencyCompensator.setPitchPreserving(!bypassed);
    for (int offset = 0; offset < samples; offset += sliceCapacity)
    {
        const int n = juce::jmin(sliceCapacity, samples - offset);
        std::array<float*, 2> slice { buffer.getWritePointer(0, offset), nullptr };
        if (channels > 1)
            slice[1] = buffer.getWritePointer(1, offset);

        // Capture original dry audio before any pitch shifting, and align it
        // with the advertised host latency. Delay state advances during both
        // bypass and active processing, so automation never cold-starts it.
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c)
            {
                // Sanitize before BOTH the delayed dry/bypass ring and the
                // pitch tracker. Otherwise a single invalid DAW sample can
                // persist in the PDC bypass line even after the DSP recovers.
                if (! std::isfinite (slice[(size_t)c][i])) slice[(size_t)c][i] = 0.0f;
                dryLatencyScratch[(size_t)c * (size_t)n + (size_t)i]
                    = latencyAlignedBypass.delayDry(c, slice[(size_t)c][i]);
            }

        realtimeAutotune.processChannels(slice.data(), channels, n, intrinsicLatencyScratch.data());
        processDoubler(buffer, offset, n);
        outputLatencyCompensator.processChannels(slice.data(), channels, n, intrinsicLatencyScratch.data());
        // Reference piano is generated locally, so it needs exactly the host
        // budget, NOT the voice's pitch-dependent delay-tap compensation.
        processReferenceTone(buffer, offset, n);

        // Wet and dry now occupy the same host-aligned timeline. Do this after
        // all effects so bypass also suppresses the doubler and preview tone.
        for (int i = 0; i < n; ++i)
        {
            latencyAlignedBypass.advance();
            for (int c = 0; c < channels; ++c)
                slice[(size_t)c][i] = latencyAlignedBypass.mix(
                    slice[(size_t)c][i],
                    dryLatencyScratch[(size_t)c * (size_t)n + (size_t)i]);
        }
    }

    const float detected = realtimeAutotune.getDetectedMidi();
    const float target = (float)realtimeAutotune.getTargetMidi();
    const float correction = realtimeAutotune.getCorrectionSemitones();
    const float confidence = realtimeAutotune.getConfidence();
    const float hz = detected >= 0.0f
        ? settings.referenceHz * std::exp2((detected - 69.0f) / 12.0f) : 0.0f;
    detectedMidiNote.store(detected >= 0.0f ? detected : 0.0f, std::memory_order_relaxed);
    detectedHz.store(hz, std::memory_order_relaxed);
    pitchConfidence.store(confidence, std::memory_order_relaxed);
    targetMidiNote.store(target >= 0.0f ? target : (detected >= 0.0f ? detected : 69.0f), std::memory_order_relaxed);
    smoothedTargetMidiNote.store(target >= 0.0f ? target : 69.0f, std::memory_order_relaxed);
    correctionCents.store(correction * 100.0f, std::memory_order_relaxed);
    effectiveRetuneTimeMs.store(settings.retuneMs, std::memory_order_relaxed);

}

void MetaTuneInspiredAudioProcessor::processReferenceTone(juce::AudioBuffer<float>& buffer, int startSample, int samples) noexcept
{
    const int channels = juce::jmin(2, buffer.getNumChannels());
    if (channels <= 0 || samples <= 0)
        return;

    const bool globallyBypassed = globalBypassParameter != nullptr
        && globalBypassParameter->load(std::memory_order_relaxed) > 0.5f;
    const int note = globallyBypassed ? -1 : tonePreviewMidiNote.load(std::memory_order_relaxed);
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
    std::array<float*, 2> outputPointers { buffer.getWritePointer(0, startSample), nullptr };
    if (channels > 1)
        outputPointers[1] = buffer.getWritePointer(1, startSample);
    for (int i = 0; i < samples; ++i)
    {
        if (note >= 0)
            referenceToneFrequencyHz += (targetFrequency - referenceToneFrequencyHz) * frequencyAlpha;
        const float alpha = targetAmplitude > referenceToneAmplitude ? attackAlpha : releaseAlpha;
        referenceToneAmplitude += (targetAmplitude - referenceToneAmplitude) * alpha;
        if (referenceToneAmplitude < 1.0e-6f && targetAmplitude == 0.0f)
            referenceToneAmplitude = 0.0f;
        const float tone = referenceToneAmplitude > 1.0e-6f
            ? std::sin(referenceTonePhase) * referenceToneAmplitude : 0.0f;
        const float delayedTone = referenceToneDelay.processSample(tone);
        for (int channel = 0; channel < channels; ++channel)
            outputPointers[(size_t)channel][i] += delayedTone;
        referenceTonePhase += juce::MathConstants<float>::twoPi * referenceToneFrequencyHz
                            / (float)currentSampleRate;
        if (referenceTonePhase >= juce::MathConstants<float>::twoPi)
            referenceTonePhase -= juce::MathConstants<float>::twoPi;
    }
}

void MetaTuneInspiredAudioProcessor::processDoubler(juce::AudioBuffer<float>& buffer, int startSample, int samples) noexcept
{
    const int channels = juce::jmin(2, buffer.getNumChannels());
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
    auto* left = buffer.getWritePointer(0, startSample);
    auto* right = channels > 1 ? buffer.getWritePointer(1, startSample) : nullptr;
    auto readDelay = [this](int channel, float delaySamples) noexcept -> float
    {
        float readPosition = (float)doublerWritePosition - delaySamples;
        if (readPosition < 0.0f) readPosition += (float)doublerDelayLineSize;
        if (readPosition >= (float)doublerDelayLineSize) readPosition -= (float)doublerDelayLineSize;
        const int i1 = (int)readPosition;
        const int i0 = (i1 - 1 + doublerDelayLineSize) % doublerDelayLineSize;
        const int i2 = (i1 + 1) % doublerDelayLineSize;
        const int i3 = (i1 + 2) % doublerDelayLineSize;
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
        doublerWritePosition = (doublerWritePosition + 1) % doublerDelayLineSize;
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
