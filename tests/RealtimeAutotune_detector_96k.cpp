// Verify the higher-rate decimator reports stable F0 while rejecting noise.
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <algorithm>
int main()
{
    constexpr double pi = 3.14159265358979323846;
    for (double sr : {44100., 48000., 88200., 96000., 192000.})
        for (double f0 : {85., 120., 220., 440., 780.})
        {
            autotune::YinDetector detector; detector.prepare(sr);
            double phase = 0.;
            double sumHz = 0.; int count = 0;
            for (int i=0; i<(int)(sr*.25);++i)
            {
                const float x = (float)(.21*std::sin(phase) + .07*std::sin(2.*phase + .5));
                phase += 2.*pi*f0/sr;
                if(detector.push(x) && i > (int)(sr*.12) && detector.getConfidence() > .8f)
                    {sumHz += detector.getFrequency(); ++count;}
            }
            const double cents = count ? 1200.*std::log2((sumHz / count)/f0) : 10000.;
            if(count < 10 || std::abs(cents)>15.)
            {
                std::fprintf(stderr,"Bad F0 sr %.0f f %.0f cents %.2f count %d\n",sr,f0,cents,count);
                return 1;
            }
        }
    // Broadband stationary noise must not repeatedly masquerade as a note.
    autotune::YinDetector detector; detector.prepare(96000.);
    uint32_t seed=123456;int suspect=0;
    for(int i=0;i<96000;++i)
    {
        seed ^= seed<<13;seed ^= seed>>17;seed ^= seed<<5;
        const float x = .2f*((seed&65535u)/32767.5f - 1.f);
        if(detector.push(x) && detector.getConfidence()>.85f && detector.getFrequency()>0.f) ++suspect;
    }
    if(suspect > 4) {std::fprintf(stderr,"High-confidence noise false notes: %d\n",suspect);return 2;}
    std::printf("PASS 25 pitch cases at 5 rates and 96k noise gate\n");
}
