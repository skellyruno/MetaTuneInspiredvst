// Multi-rate corrected-note characterization. Prints CSV to stdout.
// This is a synthetic regression gate, not a claim of full vocal/audio quality.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi=3.14159265358979323846;

double pitch(const std::vector<float>& signal,int begin,int end,double sr) {
    double first=0.,last=0.; int n=0;
    for(int i=std::max(begin+1,1);i<end;++i) {
        const float a=signal[(size_t)i-1],b=signal[(size_t)i];
        if(a<=0 && b>0 && b!=a) {
            const double t=(double)i-1.0-(double)a/((double)b-a);
            if(n++==0) first=t;
            last=t;
        }
    }
    return n>1 && last>first ? (n-1)*sr/(last-first) : 0.;
}

struct Result { int target=-1;double cents=0,medianMs=0,p95Ms=0,rms=0; };
double quantile(std::vector<double> x,double q) {
    if(x.empty()) return 0.;
    auto it=x.begin()+(std::ptrdiff_t)((x.size()-1)*q);
    std::nth_element(x.begin(),it,x.end());
    return *it;
}
Result measure(double sr,double hz,bool low) {
    autotune::RealtimeAutotune e;
    e.prepare(sr);
    autotune::Settings s;
    s.key=0;s.scale=autotune::Scale::Major;s.strength=1;s.retuneMs=0;
    s.voicingThreshold=0.5f;s.lowLatency=low;s.follow=1;s.noteStabilizerMs=0;
    e.setSettings(s);e.reset();
    const int n=(int)(sr*0.9);
    std::vector<float> out((size_t)n);
    std::vector<double> delays;
    delays.reserve((size_t)sr);
    double a=0.,b=0.;
    for(int i=0;i<n;++i) {
        const float x=0.24f*(float)std::sin(2.0*pi*hz*(double)i/sr);
        out[(size_t)i]=e.process(x);
        if(i>(int)(sr*.30) && e.isVoiced()) delays.push_back(e.getLatencySamples()/sr*1000.);
        if(i>(int)(sr*.45)) { a+=(double)x*x;b+=(double)out[(size_t)i]*out[(size_t)i]; }
    }
    Result r;
    r.target=e.getTargetMidi();
    const double predicted=pitch(out,(int)(sr*.45),n,sr);
    const double wanted=440.0*std::exp2((r.target-69.)/12.);
    r.cents=predicted>0. && r.target>=0 ? 1200.*std::log2(predicted/wanted):9999.;
    r.medianMs=quantile(delays,.50);r.p95Ms=quantile(delays,.95);
    r.rms=std::sqrt(b/std::max(1e-15,a));
    return r;
}
}

int main() {
    std::puts("sample_rate,mode,input_hz,target_midi,error_cents,median_delay_ms,p95_delay_ms,rms_ratio,status");
    int outliers=0;
    for(double sr:{44100.,48000.,96000.}) {
        for(bool low:{false,true}) {
            for(double hz:{77.78,92.5,116.54,155.56,185.,233.08,275.,300.,420.,466.16}) {
                const Result r=measure(sr,hz,low);
                const bool ok=std::isfinite(r.cents)&&std::abs(r.cents)<25.0&&r.rms>.75&&r.rms<1.25&&r.p95Ms<36.;
                if(!ok) ++outliers;
                std::printf("%.0f,%s,%.2f,%d,%+.2f,%.2f,%.2f,%.3f,%s\n",sr,low?"low":"normal",hz,r.target,r.cents,r.medianMs,r.p95Ms,r.rms,ok?"ok":"outlier");
            }
        }
    }
    std::fprintf(stderr,"Quality-grid diagnostic: %d outliers among 60 synthetic steady tones (fail on outliers; not a real-vocal quality guarantee).\n",outliers);
    return outliers == 0 ? 0 : 1;
}
