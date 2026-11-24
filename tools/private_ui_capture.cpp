// Native actual JUCE editor capture, never a screenshot composite.
// All proprietary resources are installed only after the executable is downloaded.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

int main(int argc,char**argv)
{
    const bool requirePrivate=argc>1 && juce::String(argv[1])=="--require-private";
    juce::ScopedJuceInitialiser_GUI init;
    MetaTuneInspiredAudioProcessor processor;
    processor.prepareToPlay(48000.0,256);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    if (!editor) return 2;
    editor->setVisible(true);
    bool privateLoaded=false;
    for (int i=0;i<editor->getNumChildComponents();++i)
        if (auto* child=editor->getChildComponent(i))
            if (child->getName()=="Licensed resource audit") privateLoaded=true;
    std::printf("private_skin_loaded=%d\n",privateLoaded?1:0);
    if (requirePrivate && !privateLoaded) return 3;
    {
        MetaTuneInspiredAudioProcessor follower;
        processor.setNoteGroup(1);follower.setNoteGroup(1);
        if(MetaTuneInspiredAudioProcessor::getNoteGroupCount(1)!=2)return 12;
        auto* root=processor.parameters.getParameter("root");
        auto* scale=processor.parameters.getParameter("scale");
        if(!root||!scale)return 13;
        root->setValueNotifyingHost(root->convertTo0to1(7));
        scale->setValueNotifyingHost(scale->convertTo0to1(6));
        processor.publishGroupKeyScale();follower.syncGroupKeyScale();
        if(juce::roundToInt(follower.parameters.getRawParameterValue("root")->load())!=7||
           juce::roundToInt(follower.parameters.getRawParameterValue("scale")->load())!=6)return 14;
        follower.setNoteGroup(0);
        root->setValueNotifyingHost(root->convertTo0to1(4));
        scale->setValueNotifyingHost(scale->convertTo0to1(0));
        processor.publishGroupKeyScale();follower.syncGroupKeyScale();
        if(juce::roundToInt(follower.parameters.getRawParameterValue("root")->load())!=7)return 15;
        processor.setNoteGroup(0);
        if(MetaTuneInspiredAudioProcessor::getNoteGroupCount(1)!=0)return 16;
        std::printf("group_link_and_leave_passed=1\n");
    }
    const auto output=juce::File::getCurrentWorkingDirectory().getChildFile("ui_shots");
    if (output.createDirectory().failed()) return 4;

    auto capture=[&](const juce::String& name)->bool {
        juce::Image image(juce::Image::ARGB,editor->getWidth(),editor->getHeight(),true);
        juce::Graphics graphics(image);
        editor->paintEntireComponent(graphics,true);
        auto stream=output.getChildFile(name).createOutputStream();
        juce::PNGImageFormat format;
        return stream!=nullptr && format.writeImageToStream(image,*stream);
    };
    auto click=[&](float nativeX,float nativeY,int expected)->bool {
        // Pixel coordinates refer to the original high-resolution 1700x1344 shell.
        juce::Point<float> p(nativeX*editor->getWidth()/1700.0f,
                             nativeY*editor->getHeight()/1344.0f);
        auto source=juce::Desktop::getInstance().getMainMouseSource();
        auto now=juce::Time::getCurrentTime();
        juce::MouseEvent down(source,p,juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                             1.0f,0,0,0,0,editor.get(),editor.get(),now,p,now,1,false);
        editor->mouseDown(down);
        juce::MouseEvent up(source,p,juce::ModifierKeys(),0.0f,0,0,0,0,
                           editor.get(),editor.get(),now,p,now,1,false);
        editor->mouseUp(up);
        const auto* root=processor.parameters.getRawParameterValue("root");
        const int actual=root!=nullptr?juce::roundToInt(root->load()):-1;
        if (actual!=expected)
            std::fprintf(stderr,"Note hit failed at %.0f,%.0f expected %d actual %d\n",
                         nativeX,nativeY,expected,actual);
        return actual==expected;
    };

    for (auto size : {juce::Point<int>(683,540),juce::Point<int>(1000,790),
                      juce::Point<int>(1366,1080)})
    {
        editor->setSize(size.x,size.y);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        for(int i=0;i<editor->getNumChildComponents();++i)
        {
            auto* child=editor->getChildComponent(i);
            if(child!=nullptr && child->isVisible() &&
               !editor->getLocalBounds().contains(child->getBounds())) return 5;
        }
        if(privateLoaded)
        {
            // Each natural in the bottom region, each raised sharp, and
            // every lane of the original upper 12-note audition strip.
            const std::array<int,7> naturals{0,2,4,5,7,9,11};
            const std::array<float,7> naturalX{290,470,650,835,1020,1210,1400};
            const std::array<int,5> sharps{1,3,6,8,10};
            const std::array<float,5> sharpX{365,580,904,1119,1335};
            for(size_t i=0;i<naturals.size();++i)
                if(!click(naturalX[i],1290,naturals[i])) return 6;
            for(size_t i=0;i<sharps.size();++i)
                if(!click(sharpX[i],1160,sharps[i])) return 7;
            for(int n=0;n<12;++n)
                if(!click(206.0f+(n+0.5f)*1288.0f/12.0f,1015,n)) return 8;
            if(!click(650,1290,4)) return 9;
            std::printf("keyboard_hits_passed=%dx%d (24 original regions)\n",size.x,size.y);
        }
        if(!capture("MetaTune_"+juce::String(size.x)+"x"+juce::String(size.y)+".png")) return 10;
    }
    if(privateLoaded)
    {
        const auto tap=[&](float x,float y){
            juce::Point<float> p(x*editor->getWidth()/1366.0f,y*editor->getHeight()/1080.0f);
            auto source=juce::Desktop::getInstance().getMainMouseSource();auto now=juce::Time::getCurrentTime();
            juce::MouseEvent down(source,p,juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),1,0,0,0,0,editor.get(),editor.get(),now,p,now,1,false);
            editor->mouseDown(down);
            juce::MouseEvent up(source,p,juce::ModifierKeys(),0,0,0,0,0,editor.get(),editor.get(),now,p,now,1,false);editor->mouseUp(up);
        };
        tap(103,30);if(!capture("MetaTune_preset_browser.png"))return 40;
        tap(291,98);if(!capture("MetaTune_group_browser.png"))return 41;
        tap(220,301);if(processor.getNoteGroup()!=1)return 42;
        tap(220,231);if(processor.getNoteGroup()!=0)return 43;
        tap(103,30);tap(80,920);
        if(!capture("MetaTune_scale_browser.png"))return 44;
        tap(121,742);if(!capture("MetaTune_scale_browser_flats.png"))return 45;
    }
    if(auto* speed=processor.parameters.getParameter("speed"))
        speed->setValueNotifyingHost(1.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(450);
    if(!capture("MetaTune_negative_retune.png")) return 11;
    editor.reset();
    processor.releaseResources();
    return 0;
}
