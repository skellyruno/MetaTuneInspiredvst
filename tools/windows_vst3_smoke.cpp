// Run on a native Windows runner after the JUCE VST3 build. This is a host
// integration smoke test, not a substitute for FL Studio or a plugin validator.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

int main(int argc,char**argv)
{
#if !defined(_WIN32)
    std::fprintf(stderr,"Windows-only smoke check\n");return 1;
#else
    if(argc!=2){std::fprintf(stderr,"usage: SkellyVST3Smoke <plugin.vst3>\n");return 2;}
    juce::ScopedJuceInitialiser_GUI initialiser;
    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    const juce::String pluginPath=juce::String::fromUTF8(argv[1]);
    vst3.findAllTypesForFile(descriptions,pluginPath);
    if(descriptions.isEmpty())
    {
        std::fprintf(stderr,"VST3 scan returned no descriptions: %s\n",argv[1]);
        return 3;
    }
    for(double sampleRate:{44100.0,48000.0,96000.0})
    {
        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> plugin(
            vst3.createInstanceFromDescription(*descriptions[0],sampleRate,256,error));
        if(!plugin)
        {
            std::fprintf(stderr,"VST3 create failed @ %.0f: %s\n",sampleRate,error.toRawUTF8());
            return 4;
        }
        plugin->prepareToPlay(sampleRate,256);
        if(plugin->getTotalNumInputChannels()<1 || plugin->getTotalNumOutputChannels()<1)
            return 5;
        auto runAudio=[&](int blocks)->bool
        {
            juce::AudioBuffer<float> buffer(2,256);
            juce::MidiBuffer midi;
            for(int b=0;b<blocks;++b)
            {
                for(int c=0;c<2;++c)
                    for(int i=0;i<256;++i)
                    {
                        const double t=(double)(b*256+i)/sampleRate;
                        buffer.setSample(c,i,(float)(.15*std::sin(2.*juce::MathConstants<double>::pi*195.0*t)));
                    }
                plugin->processBlock(buffer,midi);
                for(int c=0;c<2;++c)
                    for(int i=0;i<256;++i)
                        if(!std::isfinite(buffer.getSample(c,i)) ||
                           std::abs(buffer.getSample(c,i))>16.0f)
                            return false;
            }
            return true;
        };
        // Verify real VST3 instance, state save/reload, parameter automation
        // and processBlock without crashes or nonfinite stereo samples.
        if(!runAudio(80))return 6;
        juce::MemoryBlock state;
        plugin->getStateInformation(state);
        if(state.isEmpty())return 7;
        for(auto* parameter:plugin->getParameters())
        {
            const auto name=parameter->getName(128);
            if(name.containsIgnoreCase("Low Latency") ||
               name.containsIgnoreCase("Global Bypass"))
            {
                parameter->setValueNotifyingHost(0.f);
                if(!runAudio(10))return 8;
                parameter->setValueNotifyingHost(1.f);
                if(!runAudio(10))return 9;
            }
        }
        plugin->setStateInformation(state.getData(),(int)state.getSize());
        if(!runAudio(25))return 10;
        // The processor deliberately notifies the host of PDC on JUCE's
        // message-thread timer, not from the real-time callback. This test
        // pumps that message loop and reads PDC from the ACTUAL VST3 instance.
        auto* liveParam = static_cast<juce::AudioProcessorParameter*>(nullptr);
        auto* legacyParam = static_cast<juce::AudioProcessorParameter*>(nullptr);
        auto* bypassParam = static_cast<juce::AudioProcessorParameter*>(nullptr);
        for(auto* parameter:plugin->getParameters())
        {
            const auto name=parameter->getName(128);
            if(name.equalsIgnoreCase("Low Latency"))liveParam=parameter;
            if(name.containsIgnoreCase("Legacy Fixed Mode"))legacyParam=parameter;
            if(name.containsIgnoreCase("Global Bypass"))bypassParam=parameter;
        }
        if(!liveParam || !bypassParam || !legacyParam)return 11;
        for (bool live : {true,false})
        {
            liveParam->setValueNotifyingHost(live?1.f:0.f);
            // A V1.2 project can recall this ON. Production builds must ignore it.
            legacyParam->setValueNotifyingHost(1.f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
            const int budget=(int)std::ceil(sampleRate*(live?0.0035:0.008));
            if (plugin->getLatencySamples()!=budget ||
                (double)budget/sampleRate*1000.0>10.0)
            {
                std::fprintf(stderr,"Strict host PDC @ %.0f live=%d: wanted %d got %d\n",
                    sampleRate,(int)live,budget,plugin->getLatencySamples());
                return 12;
            }
            if(!runAudio(75))return 13;
            bypassParam->setValueNotifyingHost(1.f);
            juce::AudioBuffer<float> impulseBuffer(2,256);
            juce::MidiBuffer impulseMidi;
            for(int b=0;b<75;++b)
            {
                impulseBuffer.clear();
                plugin->processBlock(impulseBuffer,impulseMidi);
            }
            int firstHit=-1;
            for(int blockStart=0;blockStart<budget+1024;blockStart+=256)
            {
                impulseBuffer.clear();
                if(blockStart==0)
                    for(int c=0;c<2;++c) impulseBuffer.setSample(c,0,1.f);
                plugin->processBlock(impulseBuffer,impulseMidi);
                for(int i=0;i<256;++i)
                {
                    const float v=impulseBuffer.getSample(0,i);
                    if(!std::isfinite(v))return 14;
                    if(std::abs(v)>0.6f && firstHit<0)firstHit=blockStart+i;
                }
            }
            if(firstHit!=budget)
            {
                std::fprintf(stderr,"Strict bypass impulse @ %.0f live=%d: PDC=%d hit=%d\n",
                    sampleRate,(int)live,budget,firstHit);
                return 15;
            }
            bypassParam->setValueNotifyingHost(0.f);
            if(!runAudio(30))return 16;
            std::printf("PASS native VST3 %.0f Hz Live=%d PDC=%d bypass-impulse=%d\n",
                        sampleRate,(int)live,budget,firstHit);
        }
        legacyParam->setValueNotifyingHost(0.f);
        plugin->releaseResources();
    }
    return 0;
#endif
}
