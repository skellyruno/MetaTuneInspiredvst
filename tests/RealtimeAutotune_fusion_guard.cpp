#include "FusionPeriodGuard.h"
#include <cassert>
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstdio>

using autotune::FusionPeriodGuard;
constexpr double pi = 3.14159265358979323846;

static void checkRate(double sr, int block) {
    FusionPeriodGuard guard;
    guard.prepare(sr);
    const int samples = (int)(sr * 0.25);
    std::vector<float> a(samples), b(samples);
    for (int i = 0; i < samples; ++i) {
        a[i] = (float)(0.36 * std::sin(2.0 * pi * 220.0 * i / sr));
        b[i] = -a[i]; // anti-phase stereo must not cancel the verifier
    }
    for (int offset=0; offset < samples; offset += block) {
        const float* in[] = {a.data()+offset, b.data()+offset};
        guard.push(in, 2, std::min(block, samples-offset));
    }
    auto good=guard.inspect(220.0f);
    assert(good.available && good.periodScore > 0.93f && good.twoPeriodScore > 0.93f);
    auto bad=guard.inspect(440.0f);
    assert(bad.available && bad.periodScore < -0.80f && bad.twoPeriodScore > 0.90f);
    guard.observe(1,220.0f,0.99f,0.01f,true,256);
    assert(guard.guardedNoteConfirmMs(10.0f,true,false,20.0f)==10.0f);
    for (int n=2; n<10; ++n) guard.observe((unsigned)n,220.0f,0.55f,0.9f,true,(int)(sr*0.004));
    assert(guard.guardedNoteConfirmMs(10.0f,true,false,20.0f)>10.0f);
    assert(guard.guardedNoteConfirmMs(0.0f,true,false,20.0f)==0.0f);
    assert(guard.guardedNoteConfirmMs(10.0f,true,true,20.0f)==10.0f);
    assert(guard.guardedNoteConfirmMs(10.0f,true,false,0.0f)==10.0f);
    assert(guard.guardedNoteConfirmMs(10.0f,true,false,20.0f)<=20.0f);
    guard.reset();
    assert(!guard.inspect(220.0f).available);
}

int main() {
    for (double rate: {44100.0,48000.0,96000.0})
        for (int block: {32,64,128,256,1024}) checkRate(rate,block);
    std::puts("FusionPeriodGuard: all deterministic periodicity and safety tests passed");
}
