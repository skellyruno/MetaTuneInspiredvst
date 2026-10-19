// Allocation-trap audio callback regression. Checks source-side real-time safety
// under live automation, stereo processing and mode/bypass transitions.
#include "RealtimeAutotune.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <array>
static std::atomic<bool> inCallback {false};
static std::atomic<unsigned> allocations {0};
void* operator new(std::size_t n)
{
    if (inCallback.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* p=std::malloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

int main()
{
    autotune::RealtimeAutotune tune;
    tune.prepare(96000.0);
    autotune::Settings settings;
    std::array<float,128> a{},b{};
    std::array<float,256> intrinsic{};
    float* channels[]={a.data(),b.data()};
    for(int block=0; block<300; ++block)
    {
        if(block%20==0)
        {
            settings.lowLatency = (block%40)==0;
            settings.bypass = (block%60)==20;
            settings.retuneMs = (float)(block%100);
            settings.glideMs = (float)(block%70);
            tune.setSettings(settings);
        }
        for(int i=0;i<128;++i)
        {
            const double t=(block*128+i)/96000.0;
            a[(size_t)i]=(float)(.17*std::sin(2*3.141592653589793*220*t));
            b[(size_t)i]=a[(size_t)i]*-.72f;
        }
        inCallback.store(true,std::memory_order_relaxed);
        tune.processChannels(channels,2,128,intrinsic.data());
        inCallback.store(false,std::memory_order_relaxed);
        for(int i=0;i<128;++i)
            if(!std::isfinite(a[(size_t)i]) || !std::isfinite(b[(size_t)i]))
                {std::fprintf(stderr,"Nonfinite audio callback output\n");return 2;}
    }
    // V7: trigger the phase-matched unity transition while an allocation
    // trap is active. Preparation is explicitly outside the callback.
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (bool fast : {true, false})
        {
            autotune::PsolaShifter shift;
            shift.prepare(rate);
            shift.setLowLatency(fast);
            double phase = 0.0;
            for (int i=0; i < (int)(rate*0.32); ++i)
            {
                const double t = (double)i / rate;
                const double ratio = t < .13 ? 1.035 : (t < .19 ? 1.0 : 1.035);
                shift.setParameters((float)ratio,(float)(rate/440.0));
                phase += 2.0 * 3.141592653589793 * 440.0 / rate;
                const float input = (float)(0.2 * std::sin(phase) + 0.09 * std::sin(2.0*phase));
                inCallback.store(true,std::memory_order_relaxed);
                const float output = shift.processSample(input);
                inCallback.store(false,std::memory_order_relaxed);
                if (!std::isfinite(output)) return 3;
            }
        }
    // V12: the host-aligned dry branch must also stay allocation-free when a
    // session automates Global Bypass repeatedly at sample/block boundaries.
    autotune::LatencyAlignedBypass aligned;
    aligned.prepare(1714, 48000.0); // allocations only before entering callback
    for (int block=0; block<300; ++block)
    {
        aligned.setBypassed((block % 7) < 3);
        inCallback.store(true,std::memory_order_relaxed);
        for (int i=0; i<128; ++i)
        {
            const float input = .2f * std::sin((float)((block*128+i) *
                              (2.0*3.141592653589793*330.0/48000.0)));
            const float dry = aligned.delayDry(0,input);
            aligned.advance();
            const float y = aligned.mix(.8f*dry,dry);
            if (!std::isfinite(y)) { inCallback.store(false,std::memory_order_relaxed); return 4; }
        }
        inCallback.store(false,std::memory_order_relaxed);
    }
    const unsigned count=allocations.load();
    if(count) {std::fprintf(stderr,"Heap allocations inside callback: %u\n",count);return 1;}
    std::puts("PASS 300 stereo callbacks, six phase handoffs, 300 PDC-aligned bypass blocks: no audio-thread heap allocation");
}
