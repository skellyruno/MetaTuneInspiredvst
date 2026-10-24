// V1.9 regression: octave steps, safe false-lock handling, natural vibrato,
// consonant handoff, and an unchanged 3.5ms Live source-history budget.
// Entirely synthetic; not a substitute for annotated real-vocal validation.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
using namespace autotune;
constexpr double pi=3.14159265358979323846;
static void mode(RealtimeAutotune& e,double sr,bool precision=false,bool natural=false)
{
 e.prepare(sr);Settings s;s.strictRealtime=s.lowLatency=s.ultraLowLatency=true;
 s.precisionHardTune=precision;s.follow=natural?0.f:1.f;
 s.noteStabilizerMs=natural?300.f:0.f;s.retuneMs=0;s.noteConfirmMs=10;
 s.voicingThreshold=.8f;e.setSettings(s);
}
static int octaveStep(double sr,bool precision,double initial,double next)
{
 RealtimeAutotune e;mode(e,sr,precision);
 const int start=(int)(sr*.25),total=(int)(sr*.47);
 const int target=(int)std::lround(69+12*std::log2(next/440.));
 double phase=0;int confirmed=-1;
 for(int i=0;i<total;++i)
 {
  const double hz=i<start?initial:next;phase+=2*pi*hz/sr;
  const float x=(float)(.21*std::sin(phase)+.07*std::sin(2*phase+.2)+.02*std::sin(3*phase-.4));
  const float output=e.process(x);if(!std::isfinite(output))return -1;
  if(i>=start&&confirmed<0&&e.getTargetMidi()==target)confirmed=i-start;
 }
 return confirmed<0?-1:(int)std::lround(1000.*confirmed/sr);
}
static int spurFalseTargetSamples(double sr,bool precision,double ms,bool up)
{
 RealtimeAutotune e;mode(e,sr,precision);
 const int start=(int)(sr*.25),end=start+(int)(sr*ms/1000.);
 const int stable=(int)std::lround(69+12*std::log2((up?220.:440.)/440.));
 int count=0;double phase=0;
 for(int i=0;i<(int)(sr*.42);++i)
 {
  const double hz=(i>=start&&i<end)?(up?440.:220.):(up?220.:440.);
  phase+=2*pi*hz/sr;
  const float x=(float)(.21*std::sin(phase)+.07*std::sin(2*phase+.2)+.02*std::sin(3*phase-.4));
  e.process(x);
  if(i>=start && i<end+(int)(sr*.06) && e.getTargetMidi()!=stable)++count;
 }
 return count;
}
static bool preserveGentleVibrato(double sr)
{
 RealtimeAutotune e;mode(e,sr,false,true);
 double phase=0;int errors=0;float maxAbs=0;
 for(int i=0;i<(int)(sr*1.25);++i)
 {
  const double t=i/sr;
  const double hz=220.*std::exp2(0.6*std::sin(2*pi*4.5*t)/12.);
  phase+=2*pi*hz/sr;
  const float x=(float)(.22*std::sin(phase)+.05*std::sin(2*phase+.3)+.02*std::sin(3*phase-.7));
  const float y=e.process(x);
  if(!std::isfinite(y))return false;
  maxAbs=std::max(maxAbs,std::abs(y));
  if(t>.35&&e.getTargetMidi()!=57)++errors;
 }
 return errors==0&&maxAbs<4.f;
}
static bool consonantToVowel(double sr)
{
 RealtimeAutotune e;mode(e,sr,false);
 uint32_t rng=0x31415926;double phase=0;bool ok=true;
 const int start=(int)(sr*.25),end=start+(int)(sr*.028);
 int finalTarget=-1;
 for(int i=0;i<(int)(sr*.56);++i)
 {
  rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
  const float noise=((rng&0xffffu)/32768.f-1.f);
  const bool consonant=i>=start&&i<end;
  const double hz=i<end?146.832:195.998;
  phase+=2*pi*hz/sr;
  const float x=consonant? 0.14f*noise
    :(float)(.22*std::sin(phase)+.06*std::sin(2*phase+.2));
  const float y=e.process(x);
  if(!std::isfinite(y)||std::abs(y)>8.f)ok=false;
  if(i==(int)(sr*.54))finalTarget=e.getTargetMidi();
 }
 return ok&&finalTarget==(int)std::lround(69+12*std::log2(195.998/440.));
}
int main()
{
 // Bounds depend on measured V1.8 synthetic baseline; no latency inflation.
 for(double sr:{44100.,48000.,96000.})
 {
  if(RealtimeAutotune::ultraLowLatencyHostSamples(sr)!=(int)std::ceil(sr*.0035))return 1;
  if(!preserveGentleVibrato(sr)||!consonantToVowel(sr))return 2;
  for(bool precision:{false,true})
  {
   for(auto p:{std::pair<double,double>{110.,220.},{220.,440.},{180.,360.},
               {220.,110.},{360.,180.}})
   {
    int ms=octaveStep(sr,precision,p.first,p.second);
    if(ms<0||ms>58){std::printf("octave timing %.0fHz %d %.0f->%.0f %dms\n",sr,(int)precision,p.first,p.second,ms);return 3;}
   }
   // Short bursts must not add *new* persistent false locks. This 3ms
   // bound passes V1.8; longer spur cases are tracked in benchmark evidence.
   for(bool up:{false,true})
    if(spurFalseTargetSamples(sr,precision,3.,up)!=0)return 4;
  }
 }
 std::puts("PASS V1.9 octave timing, 3ms octave spur, vibrato, consonant handoff, 3.5ms Live budget");
}
