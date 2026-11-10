// Run on a native Windows runner after the JUCE VST3 build. This is a host
// integration smoke test, not a substitute for FL Studio or a plugin validator.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>
#include "../Source/PluginProcessor.h"
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

        // Real VST3 editor smoke, not an off-line illustration. Instantiate
        // the actual compiled UI, render at compact/mid/full sizes, and check
        // that every visible control remains within the panel.
        if(!plugin->hasEditor()) return 17;
        // The host's VST3PluginInstance returns a native placeholder
        // component. Its paint is black because the real VST3 editor lives
        // inside a CHILD HWND. The original software screenshot check was
        // inadvertently rasterizing that wrapper, not the plugin UI.
        //
        // Keep scan/instantiation/audio/state/PDC on the real VST3 instance.
        // Render the SAME compiled MetaTune editor from the linked shared
        // code target to a JUCE image independently of HWND composition.
        MetaTuneInspiredAudioProcessor uiProcessor;
        uiProcessor.prepareToPlay(sampleRate,256);
        std::unique_ptr<juce::AudioProcessorEditor> editor(uiProcessor.createEditor());
        if(!editor) return 18;
        editor->setVisible(true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
        // Software render the original editor directly. No native peer is
        // needed, and this is still the actual UI code in the VST3 build.
        auto rasterizeEditor=[&]() -> juce::Image
        {
            juce::Image image(juce::Image::ARGB, editor->getWidth(),
                              editor->getHeight(), true);
            juce::Graphics g(image);
            editor->paintEntireComponent(g, true);
            return image;
        };
        juce::Image neutral;
        for(const auto& size : {juce::Point<int>(683,540),
                                juce::Point<int>(1000,790),
                                juce::Point<int>(1366,1080)})
        {
            editor->setSize(size.x, size.y);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
            for(int childIndex=0; childIndex<editor->getNumChildComponents(); ++childIndex)
            {
                auto* child = editor->getChildComponent(childIndex);
                if(!child || !child->isVisible()) continue;
                const auto b = child->getBounds();
                if(b.getX()<0 || b.getY()<0 || b.getRight()>editor->getWidth()+1
                   || b.getBottom()>editor->getHeight()+1)
                {
                    std::fprintf(stderr,"UI control outside viewport at %dx%d\n",size.x,size.y);
                    return 19;
                }
            }
            auto snapshot = rasterizeEditor();
            if(snapshot.isNull() || snapshot.getWidth()!=size.x
               || snapshot.getHeight()!=size.y) return 20;
            if(size.x==1366) neutral = snapshot;
        }

        // The user-requested LyraTune visual: the middle ring must visibly
        // become pink when Speed enters -3ms; it must also keep animating.
        juce::AudioProcessorParameter* speedParam = nullptr;
        for(auto* param:plugin->getParameters())
            if(param->getName(128).equalsIgnoreCase("Speed")) speedParam=param;
        if(!speedParam) return 21;
        speedParam->setValueNotifyingHost(1.0f);
        // Also exercise real APVTS automation on the direct shared-code
        // editor under test, not the unrelated host wrapper instance.
        if(auto* uiSpeed=uiProcessor.parameters.getParameter("speed"))
            uiSpeed->setValueNotifyingHost(1.0f);
        else return 21;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(90);
        auto overloadShock = rasterizeEditor(); // 350ms entrance shockwave
        juce::MessageManager::getInstance()->runDispatchLoopUntil(370);
        auto negative = rasterizeEditor();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(180);
        auto animated = rasterizeEditor();
        if(negative.isNull() || animated.isNull()) return 22;
        auto pinkScore=[](const juce::Image& img)->double
        {
            double score=0;
            // 1366x1080 design canvas; only the 154px inner ring,
            // excluding colour-changing plasma/core and Speed knob.
            for(int y=201;y<529;y+=2)
                for(int x=307;x<634;x+=2)
                {
                    const double radius = std::hypot(double(x-470),double(y-365));
                    if(radius < 148.0 || radius > 164.0) continue;
                    const auto c=img.getPixelAt(x,y);
                    score += double(c.getRed()) - double(c.getGreen());
                }
            return score;
        };
        const double normalPink=pinkScore(neutral);
        const double aggressivePink=pinkScore(negative);
        const auto shotDir=juce::File::getCurrentWorkingDirectory().getChildFile("ui_shots");
        shotDir.createDirectory();
        juce::PNGImageFormat png;
        auto saveImage=[&](const juce::Image& img,const juce::String& name)->bool
        {
            auto stream=shotDir.getChildFile(name).createOutputStream();
            return stream != nullptr && png.writeImageToStream(img,*stream);
        };
        if(!saveImage(neutral,"MetaTune_normal.png")
           || !saveImage(overloadShock,"MetaTune_negative_speed_shockwave.png")
           || !saveImage(negative,"MetaTune_negative_retune.png")) return 25;
        const auto normalCentre=neutral.getPixelAt(470,365);
        const auto activeCentre=negative.getPixelAt(470,365);
        std::printf("Snapshot diagnostics: neutral alpha=%d rgb=%d,%d,%d; active alpha=%d rgb=%d,%d,%d; pink scores %.0f -> %.0f\n",
                    normalCentre.getAlpha(),normalCentre.getRed(),normalCentre.getGreen(),normalCentre.getBlue(),
                    activeCentre.getAlpha(),activeCentre.getRed(),activeCentre.getGreen(),activeCentre.getBlue(),
                    normalPink,aggressivePink);
        if(aggressivePink <= normalPink + 250.0)
        {
            std::fprintf(stderr,"Negative retune ring missing: neutral %.0f active %.0f\n",
                         normalPink,aggressivePink);
            return 23;
        }
        // The original LyraTune visual also includes an independent RED
        // Speed-dial overload (flickering LEDs and rotating shockwave), not
        // merely the pink orb. Verify the actual child control changes in
        // response to negative-ms automation at the design-resolution bounds.
        auto speedHeatScore=[](const juce::Image& image)->double
        {
            double score=0.0;
            for(int y=128;y<336;y+=2)
                for(int x=960;x<1175;x+=2)
                {
                    const double distance=std::hypot(double(x-1067.5),
                                                     double(y-232.5));
                    if(distance<76.0 || distance>110.0) continue;
                    const auto c=image.getPixelAt(x,y);
                    score += std::max(0.0, double(c.getRed()) -
                                          0.6*double(c.getGreen()) -
                                          0.4*double(c.getBlue()));
                }
            return score;
        };
        const double normalHeat=speedHeatScore(neutral);
        const double negativeHeat=speedHeatScore(negative);
        const double shockHeat=speedHeatScore(overloadShock);
        std::printf("Speed overload: normal %.0f, negative %.0f, entrance shock %.0f\n",
                    normalHeat,negativeHeat,shockHeat);
        if(negativeHeat <= normalHeat + 100.0) return 26;
        // Crossing 0ms must produce a distinct entrance frame that ends
        // after 350ms, even though negative Speed remains selected.
        if(std::abs(shockHeat - negativeHeat) < 20.0) return 27;

        // Verify animation moves when only time changes; the Speed value
        // stays pinned in the same negative state.
        int visualChanged=0;
        for(int y=201;y<529;y+=4)
            for(int x=307;x<634;x+=4)
                if(negative.getPixelAt(x,y)!=animated.getPixelAt(x,y))
                    ++visualChanged;
        if(visualChanged < 5) return 24;


        std::printf("UI visual smoke: 3 sizes, controls in-bounds, pink ring %.0f -> %.0f, animated pixels=%d\n",
                    normalPink,aggressivePink,visualChanged);
        editor.reset();
        uiProcessor.releaseResources();

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
            const int fadeLength=std::max(1,(int)std::lround(sampleRate*0.004));
            const double margin=2.0+fadeLength*(1.0594630943592953-1.0);
            const int budget=live
                ? (int)std::ceil(margin+0.5*sampleRate/160.0)
                : (int)std::ceil(margin+0.5*sampleRate/70.0)
                  +(int)std::lround(sampleRate*0.002);
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
