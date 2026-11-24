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
        auto* degree=processor.parameters.getParameter("customDegree1");
        auto* followerAmount=follower.parameters.getParameter("amount");
        if(!root||!scale||!degree||!followerAmount)return 13;
        followerAmount->setValueNotifyingHost(0.37f);
        degree->setValueNotifyingHost(1.0f);
        root->setValueNotifyingHost(root->convertTo0to1(7));
        scale->setValueNotifyingHost(scale->convertTo0to1(6));
        processor.publishGroupKeyScale();follower.syncGroupKeyScale();
        if(juce::roundToInt(follower.parameters.getRawParameterValue("root")->load())!=7||
           juce::roundToInt(follower.parameters.getRawParameterValue("scale")->load())!=6||
           follower.parameters.getRawParameterValue("customDegree1")->load()<0.5f||
           std::abs(followerAmount->getValue()-0.37f)>0.001f)return 14;
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
    auto click=[&](float nativeX,float nativeY,int expected,bool audition=false)->bool {
        // Pixel coordinates refer to the original high-resolution 1700x1344 shell.
        juce::Point<float> p(nativeX*editor->getWidth()/1700.0f,
                             nativeY*editor->getHeight()/1344.0f);
        auto source=juce::Desktop::getInstance().getMainMouseSource();
        auto now=juce::Time::getCurrentTime();
        juce::MouseEvent down(source,p,juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                             1.0f,0,0,0,0,editor.get(),editor.get(),now,p,now,1,false);
        editor->mouseDown(down);
        const int sounded=processor.getTonePreviewMidiNote();
        juce::MouseEvent up(source,p,juce::ModifierKeys(),0.0f,0,0,0,0,
                           editor.get(),editor.get(),now,p,now,1,false);
        editor->mouseUp(up);
        const auto* root=processor.parameters.getRawParameterValue("root");
        const int actual=audition?sounded-60:(root!=nullptr?juce::roundToInt(root->load()):-1);
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
            // Major Hold is a true keyboard selection mode, not a root combo.
            click(510,870,4); // enable Major Hold (source x235..275, y424..444)
            for(size_t i=0;i<naturals.size();++i)
                if(!click(naturalX[i],1290,naturals[i])) return 6;
            for(size_t i=0;i<sharps.size();++i)
                if(!click(sharpX[i],1160,sharps[i])) return 7;
            for(int n=0;n<12;++n)
                if(!click(206.0f+(n+0.5f)*1288.0f/12.0f,1015,n,true)) return 8;
            if(!click(650,1290,4)) return 9;
            click(510,870,4); // release Major Hold
            if(auto* scale=processor.parameters.getParameter("scale"))scale->setValueNotifyingHost(0.0f);
            std::printf("keyboard_hits_passed=%dx%d (24 original regions)\n",size.x,size.y);
        }
        if(!capture("MetaTune_"+juce::String(size.x)+"x"+juce::String(size.y)+".png")) return 10;
    }
    if(privateLoaded)
    {
        const auto tap=[&](float x,float y,bool right=false){
            juce::Point<float> p(x*editor->getWidth()/1366.0f,y*editor->getHeight()/1080.0f);
            auto source=juce::Desktop::getInstance().getMainMouseSource();auto now=juce::Time::getCurrentTime();
            juce::MouseEvent down(source,p,juce::ModifierKeys(right?juce::ModifierKeys::rightButtonModifier:juce::ModifierKeys::leftButtonModifier),1,0,0,0,0,editor.get(),editor.get(),now,p,now,1,false);
            editor->mouseDown(down);
            juce::MouseEvent up(source,p,juce::ModifierKeys(),0,0,0,0,0,editor.get(),editor.get(),now,p,now,1,false);editor->mouseUp(up);
        };
        // Native preset fixture: only create an isolated new directory and
        // remove that specific fixture afterwards. No existing files touched.
        const auto library=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("MetaTuneInspired").getChildFile("Presets");
        if(library.createDirectory().failed())return 33;
        const auto fixtureDir=library.getNonexistentChildFile("_NativeBrowserQA","",true);
        if(fixtureDir.createDirectory().failed())return 34;
        struct RemoveFixture {juce::File f;~RemoveFixture(){f.deleteRecursively();}} cleanup{fixtureDir};
        const auto set=[&](const char* id,float v){
            if(auto* param=processor.parameters.getParameter(id))
                param->setValueNotifyingHost(param->convertTo0to1(v));
        };
        const auto raw=[&](const char* id){return processor.parameters.getRawParameterValue(id)->load();};
        set("root",2);set("scale",5);set("speed",81);
        juce::MemoryBlock preset;processor.getStateInformation(preset);
        const auto fixture=fixtureDir.getChildFile("Native Test Preset.mti");
        if(!fixture.replaceWithData(preset.getData(),preset.getSize()))return 35;
        set("root",4);set("scale",0);set("speed",20);
        tap(103,30);
        juce::TextEditor* search=nullptr;
        for(int i=0;i<editor->getNumChildComponents();++i)
            if(editor->getChildComponent(i)->getName()=="Preset search")
                search=dynamic_cast<juce::TextEditor*>(editor->getChildComponent(i));
        if(!search||!search->isVisible())return 36;
        search->setText(fixtureDir.getFileName(),true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        if(!capture("MetaTune_preset_browser.png"))return 40;
        tap(190,245);
        if(juce::roundToInt(raw("root"))!=2||juce::roundToInt(raw("scale"))!=5||
           std::abs(raw("speed")-81)>0.2f)return 37;
        tap(300,218); // Keep scale
        set("root",7);set("scale",6);set("speed",20);tap(190,245);
        if(juce::roundToInt(raw("root"))!=7||juce::roundToInt(raw("scale"))!=6||
           std::abs(raw("speed")-81)>0.2f)return 38;
        std::printf("preset_recall_keep_scale_passed=1\n");
        tap(291,98);if(!capture("MetaTune_group_browser.png"))return 41;
        tap(220,301);if(processor.getNoteGroup()!=1)return 42;
        tap(220,231);if(processor.getNoteGroup()!=0)return 43;
        tap(103,30);
        const auto hover=[&](float x,float y){
            juce::Point<float> pos(x*editor->getWidth()/1366.0f,y*editor->getHeight()/1080.0f);
            auto source=juce::Desktop::getInstance().getMainMouseSource();auto now=juce::Time::getCurrentTime();
            juce::MouseEvent move(source,pos,juce::ModifierKeys(),0,0,0,0,0,editor.get(),editor.get(),now,pos,now,0,false);
            editor->mouseMove(move);juce::MessageManager::getInstance()->runDispatchLoopUntil(410);
        };
        hover(80,920);if(!capture("MetaTune_scale_browser.png"))return 44;
        tap(42,742);if(!capture("MetaTune_scale_browser_flats.png"))return 45;
        for(int i=0;i<9;++i)
        {
            if(i==8)tap(80,1018);
            else tap(42+(i%2)*79,910+(i/2)*27);
            if(juce::roundToInt(raw("scale"))!=i)return 46;
        }
        hover(210,920); // leave the sidebar to fold the drawer
        if(!capture("MetaTune_scale_folded.png"))return 48;
        tap(167+42,698); // Note mode
        tap(207,950); // ordinary piano click toggles custom degree
        if(juce::roundToInt(raw("scale"))!=8)return 47;
        std::printf("nine_scale_choices_passed=1\n");
        // Original source geometry; exercise dot drag and host-parameter updates.
        set("doublerWidth",0);set("doublerMix",0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
        const float x0=80.0f*1366/850,y0=391.0f*1080/672;
        hover(x0,y0);
        if(!capture("MetaTune_doubler_hover.png"))return 50;
        const auto pos=[&](float x,float y){return juce::Point<float>(x*editor->getWidth()/1366.0f,y*editor->getHeight()/1080.0f);};
        auto source=juce::Desktop::getInstance().getMainMouseSource();auto now=juce::Time::getCurrentTime();
        const auto start=pos(x0,y0),end=pos(480.0f*1366/850,95.0f*1080/672);
        juce::MouseEvent down(source,start,juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),1,0,0,0,0,editor.get(),editor.get(),now,start,now,1,false);
        editor->mouseDown(down);
        juce::MouseEvent drag(source,end,juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),1,0,0,0,0,editor.get(),editor.get(),now,start,now,1,true);
        editor->mouseDrag(drag);
        if(raw("doublerWidth")<99.5f||raw("doublerMix")<99.5f)return 51;
        if(!capture("MetaTune_doubler_drag.png"))return 52;
        juce::MouseEvent up(source,end,juce::ModifierKeys(),0,0,0,0,0,editor.get(),editor.get(),now,start,now,1,true);
        editor->mouseUp(up);
        std::printf("doubler_drag_full_range_passed=1\n");
    }
    if(auto* speed=processor.parameters.getParameter("speed"))
        speed->setValueNotifyingHost(1.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(450);
    if(!capture("MetaTune_negative_retune.png")) return 11;
    editor.reset();
    processor.releaseResources();
    return 0;
}
