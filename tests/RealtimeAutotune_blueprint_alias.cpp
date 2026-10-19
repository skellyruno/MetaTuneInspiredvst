// An out-of-band tone must not reappear as a confident sub-kHz voice
// after 8:1 anti-aliased pitch-analysis decimation.
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
int main(){
 constexpr double pi=3.14159265358979323846;
 for(double sr:{44100.,48000.})
  for(double f:{4000.,4700.,7000.,9000.}){
    autotune::YinDetector d;d.prepare(sr);
    int suspicious=0,hops=0;
    for(int i=0;i<(int)(sr*.55);i++){
     const float x=.25f*(float)std::sin(2*pi*f*(double)i/sr);
     if(d.push(x) && i>sr*.2){++hops; if(d.getConfidence()>.8f && d.getFrequency()>0.0f)++suspicious;}
    }
    std::printf("%.0fHz input %.0fHz aliases %d/%d frames\n",sr,f,suspicious,hops);
    if(suspicious>3)return 1;
  }
 return 0;
}
