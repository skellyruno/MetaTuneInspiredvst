// Stable read-head regression across DAW sample rates and harmonic content.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr double pi = 3.14159265358979323846;
static double rmsError(double sr, double tone, bool stable, int block)
{
    const int n=(int)(sr*1.8);
    const int fixed=autotune::RealtimeAutotune::recommendedHostLatencySamples(sr,70.f,true);
    autotune::OutputLatencyCompensator comp;
    comp.prepare(fixed,sr);
    comp.setPitchPreserving(stable);
    std::vector<float> input((size_t)n),output((size_t)n),lat((size_t)block);
    for(int i=0;i<n;++i)input[(size_t)i]=.20f*(float)std::sin(2*pi*tone*(double)i/sr);
    const int pad=comp.getStableWetPaddingSamples();
    double energy=0.,sum=0.;
    for(int base=0;base<n;base+=block)
    {
        const int sz=std::min(block,n-base);
        for(int j=0;j<sz;++j)
        {
            const int i=base+j;
            output[(size_t)i]=input[(size_t)i];
            lat[(size_t)j]=(float)(.32*fixed+sr*.0020*std::sin(2*pi*3.2*i/sr));
        }
        float* ptr[]={output.data()+base};
        comp.processChannels(ptr,1,sz,lat.data());
    }
    for(int i=(int)(sr*.4);i<n;++i)
    {
        const double expected=.20*std::sin(2*pi*tone*(double)(i-pad)/sr);
        const double diff=(double)output[(size_t)i]-expected;
        sum+=diff*diff;energy+=expected*expected;
    }
    return std::sqrt(sum/std::max(energy,1e-12));
}
int main()
{
    for(double sr:{44100.,48000.,96000.})
        for(double hz:{220.,440.,660.})
        {
            const double good=rmsError(sr,hz,true,127);
            const double old=rmsError(sr,hz,false,127);
            if(good>.0002||old<.05)
            {
                std::fprintf(stderr,"FAIL rate=%.0f hz=%.0f stable=%.5f legacy=%.5f\n",sr,hz,good,old);
                return 1;
            }
            if(std::abs(good-rmsError(sr,hz,true,17))>1e-9) return 2;
            std::printf("PASS rate=%.0f hz=%.0f steady rms=%.7f legacy=%.3f\n",sr,hz,good,old);
        }
}
