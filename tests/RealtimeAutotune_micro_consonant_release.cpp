// V27 regression: a released voiced note must not time-warp the following
// breath/consonant, even when the controller's correction ratio is still stale.
// This previously failed in V26 (strong wet noise for >20 ms).
#include "RealtimeAutotune.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
int main()
{
 constexpr double pi=3.14159265358979323846;
 for(double sr:{44100.0,48000.0,96000.0})
 {
  autotune::MicroLatencyShifter sh;
  sh.prepare(sr);
  sh.setRatio(185.0f/195.0f);
  sh.setPeriod((float)(sr/195.0));
  uint32_t seed=102030u;
  const int release=(int)(sr*0.5), revoice=(int)(sr*0.85);
  double err[3]{}, energy[3]{};
  double revoicedDifference=0.0;
  const int n=(int)(sr*1.2);
  for(int i=0;i<n;++i)
  {
   if(i==release)sh.setPeriod(0.0f);
   if(i==revoice)sh.setPeriod((float)(sr/195.0));
   seed=seed*1664525u+1013904223u;
   const float noise=((int)(seed>>16)-32768)/32768.0f;
   const double ph=2.0*pi*195.0*i/sr;
   const float input=(i>=release&&i<revoice)?0.18f*noise
       :(float)(0.22*std::sin(ph)+0.13*std::sin(2.0*ph+0.5));
   const float output=sh.processSample(input);
   const float dry=sh.getLastAlignedDry();
   if(!std::isfinite(output)||!std::isfinite(dry))return 1;
   const int elapsed=i-release;
   if(elapsed>=0&&elapsed<(int)(sr*.032))
   {
    const int b=elapsed<(int)(sr*.005)?0:elapsed<(int)(sr*.015)?1:2;
    err[b]+=std::abs((double)output-dry);
    energy[b]+=std::abs((double)dry);
   }
   if(i>revoice+(int)(sr*.15))revoicedDifference+=std::abs((double)output-dry);
  }
  const double early=err[0]/energy[0], middle=err[1]/energy[1], late=err[2]/energy[2];
  std::printf("rate %.0f consonant difference early %.5f middle %.5f late %.5f resumed %.1f\n",
              sr,early,middle,late,revoicedDifference);
  // Do not expect exact transparency at sample zero: the previous note's
  // shifter state fades down smoothly to prevent a discontinuity.
  if(!(middle<0.14&&late<0.01&&revoicedDifference>30.0))return 2;
  if(sh.getMaximumDelaySamples()!=autotune::MicroLatencyShifter::hostLatencySamples(sr))return 3;
 }
 std::puts("PASS consonant release, dry alignment and correction reacquisition");
}
