// V13: processChannels applies pushed host automation once per audio block,
// even though the per-sample parameter mailbox polling was removed.
#include "RealtimeAutotune.h"
#include <array>
#include <cmath>
#include <cstdio>
int main()
{
    constexpr double fs = 48000.0;
    for (bool stereo: {false,true})
    {
        autotune::RealtimeAutotune e;
        e.prepare(fs);
        autotune::Settings s;
        s.scale = autotune::Scale::Chromatic;
        s.key = 0;
        e.setSettings(s);
        std::array<float,128> left {}, right {};
        float* ch[]={left.data(),right.data()};
        auto render = [&]() {
            for (int i=0;i<128;++i)
            {
                left[(size_t)i]=(float)(.2*std::sin(2*3.141592653589793*190.*i/fs));
                right[(size_t)i]=-left[(size_t)i];
            }
            e.processChannels(ch,stereo?2:1,128);
            for(int i=0;i<128;++i)
            {
                if (!std::isfinite(left[(size_t)i]) || !std::isfinite(right[(size_t)i]))
                    return false;
            }
            return true;
        };
        if(!render())return 1;
        s.scale=autotune::Scale::Minor;
        s.retuneMs=35.f;
        s.lowLatency=false;
        e.pushSettings(s);
        if(!render())return 2;
        auto stored=e.getSettings();
        if(stored.scale!=autotune::Scale::Minor || stored.retuneMs!=35.f || stored.lowLatency)
        {std::puts("FAIL automation not applied at block start");return 3;}
        s.scale=autotune::Scale::Major;
        s.retuneMs=0;
        s.lowLatency=true;
        e.pushSettings(s);
        if(!render())return 4;
        stored=e.getSettings();
        if(stored.scale!=autotune::Scale::Major || stored.retuneMs!=0.f || !stored.lowLatency)
        {std::puts("FAIL second automation not applied");return 5;}
    }
    std::puts("PASS mono/stereo parameter handover at callback boundaries");
    return 0;
}
