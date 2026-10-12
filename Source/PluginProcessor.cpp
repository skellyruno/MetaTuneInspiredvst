#include "PluginProcessor.h"
#include "PluginEditor.h"

MetaTuneInspiredAudioProcessor::MetaTuneInspiredAudioProcessor()
 : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
   parameters(*this, nullptr, "PARAMETERS", createParameters())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout MetaTuneInspiredAudioProcessor::createParameters()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout p;
    p.add(std::make_unique<AudioParameterFloat>("speed", "Speed", NormalisableRange<float>(0.0f, 100.0f, 0.1f), 30.0f));
    p.add(std::make_unique<AudioParameterFloat>("amount", "Amount", NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    p.add(std::make_unique<AudioParameterFloat>("sustain", "Sustain", NormalisableRange<float>(0.0f, 100.0f, 0.1f), 0.0f));
    p.add(std::make_unique<AudioParameterBool>("stabilizer", "Note Stabilizer", true));
    p.add(std::make_unique<AudioParameterBool>("lowLatency", "Low Latency", true));
    p.add(std::make_unique<AudioParameterChoice>("scale", "Scale", StringArray{"Chromatic","Major","Minor"}, 0));
    p.add(std::make_unique<AudioParameterInt>("root", "Root Note", 0, 11, 4));
    return p;
}

void MetaTuneInspiredAudioProcessor::prepareToPlay(double sr, int maxBlock)
{
    currentSampleRate = sr;
    monoScratch.setSize(1, maxBlock);
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxBlock, (juce::uint32) getTotalNumOutputChannels() };
    shifter.reset();
    shifter.prepare(spec);
    shifter.setPitchFactor(1.0f);
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
    // Lightweight autocorrelation pitch detector for the UI / correction target.
    // This is a prototype detector, not a production-grade vocal tracker.
    const int n = juce::jmin(buffer.getNumSamples(), 2048);
    if (n < 512) return;
    monoScratch.setSize(1, n, false, false, true);
    auto* dst = monoScratch.getWritePointer(0);
    for (int i = 0; i < n; ++i) {
        float s = 0.0f;
        for (int c = 0; c < buffer.getNumChannels(); ++c) s += buffer.getReadPointer(c)[i];
        dst[i] = s / (float) buffer.getNumChannels();
    }
    float rms = 0.0f;
    for (int i = 0; i < n; ++i) rms += dst[i] * dst[i];
    rms = std::sqrt(rms / (float)n);
    if (rms < 0.003f) return;

    const int minLag = juce::jmax(2, (int)(currentSampleRate / 1000.0));
    const int maxLag = juce::jmin(n / 2, (int)(currentSampleRate / 65.0));
    float best = 0.0f; int bestLag = 0;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        float corr = 0.0f, e1 = 0.0f, e2 = 0.0f;
        for (int i = 0; i < n - lag; i += 2) {
            const float a = dst[i], b = dst[i + lag];
            corr += a * b; e1 += a * a; e2 += b * b;
        }
        const float norm = corr / (std::sqrt(e1 * e2) + 1.0e-12f);
        if (norm > best) { best = norm; bestLag = lag; }
    }
    if (bestLag > 0 && best > 0.55f) {
        const float hz = (float)currentSampleRate / (float)bestLag;
        detectedHz.store(hz);
        detectedMidiNote.store(69.0f + 12.0f * std::log2(hz / 440.0f));
    }
}

void MetaTuneInspiredAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    updatePitchEstimate(buffer);

    const float note = detectedMidiNote.load();
    const int scale = (int)parameters.getRawParameterValue("scale")->load();
    const int root = (int)parameters.getRawParameterValue("root")->load();
    const float amount = parameters.getRawParameterValue("amount")->load() / 100.0f;
    const float speed = parameters.getRawParameterValue("speed")->load();

    static constexpr int major[] = {0,2,4,5,7,9,11};
    static constexpr int minor[] = {0,2,3,5,7,8,10};
    int nearest = (int)std::round(note);
    float bestDistance = 1000.0f;
    if (scale != 0) {
        for (int candidate = (int)std::floor(note) - 12; candidate <= (int)std::ceil(note) + 12; ++candidate) {
            int pc = ((candidate - root) % 12 + 12) % 12;
            bool allowed = false;
            for (int j = 0; j < 7; ++j) allowed |= pc == (scale == 1 ? major[j] : minor[j]);
            if (allowed && std::abs(candidate - note) < bestDistance) {
                bestDistance = std::abs(candidate - note); nearest = candidate;
            }
        }
    }
    const float target = note + ((float)nearest - note) * amount;
    const float cents = (target - note) * 100.0f;
    correctionCents.store(cents);

    // Pitch-shift ratio follows the nearest allowed note. This first-pass shifter is
    // block-based; use short blocks for better response and lower latency.
    const float ratio = juce::jlimit(0.5f, 2.0f, std::pow(2.0f, (target - note) / 12.0f));
    shifter.setPitchFactor(1.0f + (ratio - 1.0f) * juce::jlimit(0.0f, 1.0f, (100.0f - speed) / 100.0f));
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    shifter.process(context);
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        buffer.applyGain(c, 0, buffer.getNumSamples(), 1.0f);
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
