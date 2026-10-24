// V1.4: reproducible synthetic low-register timbre and original-F0 leakage gate.
// The DSP uses no new audio lookahead; compare against the unmodified dual-head.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi=3.14159265358979323846264338;
struct Bands {
    std::array<double,3> low{};
    std::array<double,4> energy{};
    std::array<double,3> a{};
    explicit Bands(double sr) {
        for (int k=0;k<3;++k) a[(size_t)k]=1-std::exp(-2*pi*(k==0?330.0:(k==1?1150.0:3100.0))/sr);
    }
    void push(double x) {
        double prev=x;
        for(int k=2;k>=0;--k) {
            low[(size_t)k]+=a[(size_t)k]*(x-low[(size_t)k]);
            const double band=prev-low[(size_t)k];
            energy[(size_t)k+1]+=band*band;
            prev=low[(size_t)k];
        }
        energy[0]+=prev*prev;
    }
};
struct Fourier {
    double ca=0,sa=0;
    void push(double x,double phase) {ca+=x*std::cos(phase);sa+=x*std::sin(phase);}
    double magnitude() const {return std::hypot(ca,sa);}
};
}
int main() {
    constexpr double sr=48000.0;
    double errorBefore=0,errorAfter=0;
    int cases=0;
    for (double f0 : {75.0,82.4,92.0,100.0,110.0,140.0})
        for(double r : {1.059463094,1.122462048,1.189207115}) {
            autotune::MicroLatencyShifter baseline,guard;
            baseline.prepare(sr); guard.prepare(sr);
            baseline.setLowRegisterFormantGuardEnabled(false);
            guard.setLowRegisterFormantGuardEnabled(true);
            baseline.setLowRegisterThreeHeadEnabled(false);
            guard.setLowRegisterThreeHeadEnabled(false);
            baseline.setRatio((float)r); guard.setRatio((float)r);
            baseline.setPeriod((float)(sr/f0)); guard.setPeriod((float)(sr/f0));
            Bands input(sr),original(sr),corrected(sr);
            double leakageBefore=0,leakageAfter=0;
            // Harmonic-rich synthetic vocal, NOT a substitute for real singers.
            // Three resonant formants sweep across partials as source F0 changes.
            constexpr int count=48000;
            std::array<Fourier,8> originalB{},originalA{},targetB{},targetA{};
            float previous=0;float maxJump=0;
            for(int i=0;i<count;++i) {
                const double t=(double)i/sr;
                double x=0;
                for (int k=1;k<=24;++k) {
                    const double h=k*f0;
                    const double env=0.12*std::exp(-0.5*std::pow((h-450.0)/175.0,2))
                      +0.13*std::exp(-0.5*std::pow((h-1100.0)/270.0,2))
                      +0.08*std::exp(-0.5*std::pow((h-2600.0)/390.0,2)) +0.025;
                    x+=env/std::sqrt((double)k)*std::sin(2*pi*h*t+0.02*k*k);
                }
                x*=0.35;
                const float before=baseline.processSample((float)x);
                const float after=guard.processSample((float)x);
                if(!std::isfinite(before)||!std::isfinite(after)||std::abs(after)>3) return 2;
                if(i>count/2) {
                    input.push(x); original.push(before); corrected.push(after);
                    maxJump=std::max(maxJump,std::abs(after-previous));
                    for(int j=0;j<8;++j) {
                        const int harmonic=j+3;
                        if(f0*harmonic>1900.0) break;
                        const double fundamentalPhase=2*pi*f0*harmonic*t;
                        const double targetPhase=fundamentalPhase*r;
                        originalB[(size_t)j].push(before,fundamentalPhase);
                        originalA[(size_t)j].push(after,fundamentalPhase);
                        targetB[(size_t)j].push(before,targetPhase);
                        targetA[(size_t)j].push(after,targetPhase);
                    }
                }
                previous=after;
            }
            double e0=0,e1=0;
            for(int b=0;b<4;++b) {
                const double src=std::max(input.energy[(size_t)b],1e-12);
                e0+=std::abs(10.0*std::log10(std::max(original.energy[(size_t)b],1e-12)/src));
                e1+=std::abs(10.0*std::log10(std::max(corrected.energy[(size_t)b],1e-12)/src));
            }
            e0/=4;e1/=4;
            // Conservative leakage audit: correcting spectral tilt must NOT
            // introduce a large new original-pitch component at full wet.
            // Not a pass asserting original pitch has already been eliminated.
            double ob=0,oa=0,tb=0,ta=0;
            for(int j=0;j<8;++j) {
                const int k=j+3;
                if(k*f0>1900.0 || k*f0*(r-1.0)<17.0) continue;
                ob+=originalB[(size_t)j].magnitude();
                oa+=originalA[(size_t)j].magnitude();
                tb+=targetB[(size_t)j].magnitude();
                ta+=targetA[(size_t)j].magnitude();
            }
            if(tb>0.001 && ta>0.001) {
                leakageBefore=20.0*std::log10((ob+1e-6)/tb);
                leakageAfter=20.0*std::log10((oa+1e-6)/ta);
                if(leakageAfter-leakageBefore>1.5) {
                    std::fprintf(stderr,"LEAK regression F0 %.1f ratio %.4f before %.2f after %.2f\n",f0,r,leakageBefore,leakageAfter);
                    return 3;
                }
            }
            if(maxJump>0.6) return 4;
            std::printf("F0 %.1f r %.3f envelope mean abs %.3f -> %.3f dB, leakage %.2f -> %.2f dB\n",f0,r,e0,e1,leakageBefore,leakageAfter);
            if(e1>e0+0.05) return 5;
            errorBefore+=e0;errorAfter+=e1; ++cases;
        }
    std::printf("V1.4 low-register envelope across %d cases: %.3f -> %.3f dB\n",cases,errorBefore/cases,errorAfter/cases);
    return (cases==18 && errorAfter<errorBefore*0.9) ? 0:6;
}
