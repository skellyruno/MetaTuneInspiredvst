// V1.10: a peak of sung vibrato is NOT a fresh note; a persistent fast pitch
// step IS. Synthetic tests, not a real-singer accuracy claim.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace autotune;
static constexpr double pi = 3.14159265358979323846;
static Settings naturalLive()
{
 Settings s;
 s.strictRealtime = s.lowLatency = s.ultraLowLatency = true;
 s.follow = 0.0f;
 s.noteStabilizerMs = 300.0f;
 s.retuneMs = 0.0f;
 s.noteConfirmMs = 10.0f;
 s.voicingThreshold = 0.8f;
 return s;
}
static bool vibrato(double sr, double depth, double rate)
{
 RealtimeAutotune e;
 e.prepare(sr);
 e.setSettings(naturalLive());
 const int count=(int)(sr*1.15);
 double phase=0.0;
 int wrong=0;
 for (int i=0;i<count;++i)
 {
  const double t=(double)i/sr;
  const double frequency=220.0*std::exp2(depth*std::sin(2.0*pi*rate*t)/12.0);
  phase+=2.0*pi*frequency/sr;
  const float x=(float)(.22*std::sin(phase)+.05*std::sin(2*phase+.3)+.02*std::sin(3*phase-.7));
  const float y=e.process(x);
  if(!std::isfinite(y)||std::abs(y)>8.0f)return false;
  if(t>=.31&&e.getTargetMidi()!=57)++wrong;
 }
 if (wrong) std::printf("vibrato %.0fHz depth %.2f rate %.2f: %d wrong samples\n",sr,depth,rate,wrong);
 return wrong==0;
}
static bool genuineStep(double sr, double newHz, float maximumMs)
{
 RealtimeAutotune e;
 e.prepare(sr);
 e.setSettings(naturalLive());
 const int start=(int)(sr*.25), total=(int)(sr*.40);
 const int midi=(int)std::lround(69.0+12.0*std::log2(newHz/440.0));
 double phase=0.0;
 int confirmed=-1;
 for(int i=0;i<total;++i)
 {
  const double freq=i<start?220.0:newHz;
  phase+=2*pi*freq/sr;
  const float x=(float)(.21*std::sin(phase)+.07*std::sin(2*phase+.2)+.02*std::sin(3*phase-.4));
  const float y=e.process(x);
  if(!std::isfinite(y))return false;
  if(i>=start&&e.getTargetMidi()==midi&&confirmed<0)confirmed=i-start;
 }
 const double ms=confirmed<0?-1.0:1000.0*(double)confirmed/sr;
 if(ms<0.0||ms>maximumMs)
 {
  std::printf("step %.0fHz 220->%.2fHz: %.2fms > %.2fms\n",sr,newHz,ms,maximumMs);
  return false;
 }
 return true;
}
// Reject a transient 2-semitone detector disturbance and promptly roll back
// any provisional pitch-centre movement. Bounds were compared to V1.9.
static bool transientBurst(double sr, double burstMs, int maximumWrongSamples)
{
 RealtimeAutotune e;
 e.prepare(sr);
 e.setSettings(naturalLive());
 const int start=(int)(sr*.25);
 const int end=start+(int)(sr*burstMs/1000.0);
 const int total=(int)(sr*.41);
 const int final=end+(int)(sr*.06);
 int wrong=0;
 double phase=0.0;
 for(int i=0;i<total;++i)
 {
  const double hz=(i>=start&&i<end)?246.9417:220.0;
  phase+=2*pi*hz/sr;
  const float x=(float)(.21*std::sin(phase)+.07*std::sin(2*phase+.2)+.02*std::sin(3*phase-.4));
  if(!std::isfinite(e.process(x)))return false;
  if(i>=start&&i<final&&e.getTargetMidi()!=57)++wrong;
 }
 if(wrong>maximumWrongSamples)
  std::printf("transient %.0fHz %.0fms: %d wrong samples\n",sr,burstMs,wrong);
 return wrong<=maximumWrongSamples;
}
int main()
{
 for(double sr:{44100.0,48000.0,96000.0})
 {
  if(RealtimeAutotune::ultraLowLatencyHostSamples(sr)!=(int)std::ceil(sr*.0035))return 1;
  for(double depth:{0.9,1.25})
   for(double rate:{4.5,6.5})
    if(!vibrato(sr,depth,rate))return 2;
  if(!transientBurst(sr,3.0,0))return 4;
  // 13ms tests include real data contamination near a note boundary;
  // reject the long stale-lock tail seen in the initial V1.10 attempt.
  if(!transientBurst(sr,13.0,(int)(sr*0.005)))return 5;
  // Normal natural-mode note attacks must not acquire the 200ms lag that
  // results from merely disabling the old unconditional centre snap.
  for(auto f:{233.0819,246.9417,261.6256,293.6648,440.0,195.9977})
   if(!genuineStep(sr,f,55.0f))return 3;
 }
 std::puts("PASS V1.10 vibrato lock, genuine steps, and 3.5ms Live transport budget");
}
