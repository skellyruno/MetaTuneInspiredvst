#include "RealtimeAutotune.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
int main(){
 int failures = 0;
 const double sr=48000., pi=3.14159265358979323846;
 for(bool low: {true,false}){
  autotune::PsolaShifter p; p.prepare(sr);p.setLowLatency(low);
  const int n=(int)sr;
  std::vector<float> in(n),out(n);double phase=0., randphase=0.;
  for(int i=0;i<n;i++){
   double hz=220.+2.*std::sin(2*pi*4.0*i/sr);
   phase+=2*pi*hz/sr;
   // small nonharmonic deterministic transient/noise; resynthesis should leave it alone at unison
   randphase=std::fmod(randphase*1.000001+0.6180339887,1.);
   in[i]=(float)(.24*std::sin(phase)+.12*std::sin(2*phase)+.08*std::sin(4*phase)+.015*(randphase-.5));
   p.setParameters(1.0f,(float)(sr/hz));
   out[i]=p.processSample(in[i]);
  }
  double bestE=1e20,bestCorr=0;int bestLag=0;
  for(int lag=0;lag<1200;lag++){
   double sumXY=0.,sumXX=0.,sumYY=0.;
   for(int i=(int)sr/3+1200;i<n;i++){
    auto a=(double)in[i-lag],b=(double)out[i];
    sumXY+=a*b;sumXX+=a*a;sumYY+=b*b;
   }
   if(sumXX<1e-10||sumYY<1e-10)continue;
   double corr=sumXY/std::sqrt(sumXX*sumYY);
   if(corr>bestCorr){bestCorr=corr;bestLag=lag;bestE=std::sqrt(std::max(0.,1-corr*corr));}
  }
  std::printf("%s maxCorrelation %.6f bestDelay %d residualFrac %.4f\n",low?"low":"normal",bestCorr,bestLag,bestE);
  if (bestCorr < (low ? 0.99999 : 0.9990)) ++failures;
 }
 return failures ? 1 : 0;
}
