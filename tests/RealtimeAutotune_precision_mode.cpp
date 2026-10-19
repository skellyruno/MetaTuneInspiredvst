// Precision hard tune is an explicit opt-in mode with host-state round-tripping.
// It must not silently change existing sounds when disabled.
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
int main()
{
    autotune::RealtimeAutotune engine;
    engine.prepare(48000);
    autotune::Settings s;
    s.key=9;s.scale=autotune::Scale::Major;
    s.noteConfirmMs=23.f;s.voicingThreshold=.71f;s.noteStabilizerMs=400.f;
    s.vibratoPreserve=.83f;s.follow=.3f;s.retuneMs=70.f;
    engine.setSettings(s);
    const auto old=engine.getSettings();
    if(old.precisionHardTune || old.retuneMs!=70.f || old.noteConfirmMs!=23.f) return 1;
    s.precisionHardTune=true;
    engine.setSettings(s);
    const auto x=engine.getSettings();
    if (!x.precisionHardTune || x.retuneMs!=0.f || x.noteConfirmMs!=0.f
        || x.vibratoPreserve!=0.f || x.noteStabilizerMs!=0.f
        || x.voicingThreshold!=.83f || x.follow!=1.f) return 2;
    engine.reset();
    float max=0.f;double phase=0.;
    for(int i=0;i<48000;++i)
    {
        phase+=6.283185307179586*(180.0+20.0*std::sin(i*.0004))/48000.;
        const float z=engine.process(.15f*std::sin(phase));
        if (!std::isfinite(z)) return 3;
        max=std::max(max,std::abs(z));
    }
    if(max<.005f || max>3.f) return 4;
    s.precisionHardTune=false;
    engine.setSettings(s);
    const auto restored=engine.getSettings();
    if(restored.noteConfirmMs!=23.f || restored.retuneMs!=70.f
        || restored.follow!=.3f || restored.vibratoPreserve!=.83f) return 5;
    std::puts("PASS precision opt-in/off returns all original tracking settings");
}
