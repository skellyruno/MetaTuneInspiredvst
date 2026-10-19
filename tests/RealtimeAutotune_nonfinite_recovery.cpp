// V13: malformed DAW/audio-driver samples must not poison pitch-tracker state,
// leak into zero-amount/bypassed output, or prevent subsequent voiced recovery.
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
float source(int i,double sr) noexcept {
    const double p = 2.0*pi*196.0*(double)i/sr;
    return 0.17f*(float)(std::sin(p)+0.35*std::sin(2*p));
}
}
int main() {
  constexpr double sr=48000;
  const float nan=std::numeric_limits<float>::quiet_NaN();
  const float inf=std::numeric_limits<float>::infinity();
  for (int route=0;route<3;++route) {
    autotune::RealtimeAutotune tuner;
    tuner.prepare(sr);
    autotune::Settings opts;
    opts.scale=autotune::Scale::Major;
    opts.key=0;opts.retuneMs=0;opts.voicingThreshold=.72f;
    if(route==1) opts.strength=0;
    tuner.setSettings(opts);
    double energy=0.;int hits=0;
    for(int i=0;i<(int)sr*2;++i) {
      float x=source(i,sr);
      if(i>=sr/2 && i<sr/2+18) x = (i%2==0) ? nan : inf;
      if(route==2) {
        float r=-0.65f*x;
        float* p[]={&x,&r};
        tuner.processChannels(p,2,1);
        if(!std::isfinite(x)||!std::isfinite(r)) {
          std::printf("FAIL stereo corrupted output route=%d at %d\n",route,i);return 1;
        }
      } else {
        x=tuner.process(x);
        if(!std::isfinite(x)) { std::printf("FAIL mono route=%d at %d\n",route,i);return 2; }
      }
      if(i>sr*1.4) energy+=(double)x*x;
      if(i>sr*1.3 && tuner.getConfidence()>.72f && tuner.isVoiced()) ++hits;
    }
    if(!(energy>2.0) || hits<300) {
      std::printf("FAIL did not recover route=%d energy=%f hits=%d\n",route,energy,hits);return 3;
    }
    std::printf("PASS route=%d finite and recovered pitched output\n",route);
  }
  // Protection must also live in the independent PDC/bypass helpers. They
  // can be used without the processor's input sanitizer in other hosts.
  autotune::LatencyAlignedBypass bypass;
  bypass.prepare(48, sr);
  bypass.setBypassed(true);
  autotune::OutputLatencyCompensator pdc;
  pdc.prepare(48,sr);
  for(int i=0;i<300;++i) {
    float dry=bypass.delayDry(0, i==5 ? nan : (i==6 ? inf : 0.12f));
    bypass.advance();
    float wet=i==12 ? inf : 0.12f;
    float* p[]={&wet};
    pdc.processChannels(p,1,1);
    const float mixed=bypass.mix(wet,dry);
    if(!std::isfinite(mixed) || !std::isfinite(wet) || !std::isfinite(dry)) {
      std::printf("FAIL PDC/bypass poisoned at %d\n",i);return 4;
    }
  }
  std::puts("PASS standalone PDC/dry bypass remains finite");
  return 0;
}
