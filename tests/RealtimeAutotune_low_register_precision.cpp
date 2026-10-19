// The reference project's real-time autocorrelation experiment highlighted
// that window boundaries and coarse lag bins modulate a moving F0 estimate.
// This is a targeted test of SkellTune's independent YIN full-rate refinement.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
int main(){
 constexpr double pi=3.14159265358979323846;
 int tested=0;
 for(double fs:{44100.,48000.,96000.})for(double hz:{70.,85.}){
  autotune::YinDetector detector;
  detector.prepare(fs);
  detector.setThreshold(.18f);
  double phase=0.,error=0.,maxError=0.;int estimates=0;
  for(int i=0;i<(int)(fs*.45);++i){
    double t=i/fs;
    double freq=hz*std::exp2(45.*std::sin(2*pi*3*t)/1200.);
    phase+=2*pi*freq/fs;
    const float x=(float)(.2*std::sin(phase)+.1*std::sin(2*phase+.3)
              +.065*std::sin(3*phase+.6)+.035*std::sin(4*phase+.2));
    if(detector.push(x)&&t>.22&&detector.getConfidence()>.7f&&detector.getFrequency()>0.0f){
      const double cents=std::abs(1200.*std::log2(detector.getFrequency()/freq));
      error+=cents;maxError=std::max(maxError,cents);++estimates;
    }
  }
  const double mae=error/std::max(1,estimates);
  const double threshold=hz<80.? (fs==44100.?8.0:7.5) : 8.0;
  if(estimates<35||mae>threshold||maxError>23.0){
    std::fprintf(stderr,"FAIL low register fs %.0f hz %.0f n %d MAE %.3f max %.3f\n",fs,hz,estimates,mae,maxError);
    return 2;
  }
  std::printf("PASS %.0f Hz at %.0f samples/s: MAE %.3f cents max %.3f (%d frames)\n",hz,fs,mae,maxError,estimates);
  ++tested;
 }
 return tested==6?0:1;
}
