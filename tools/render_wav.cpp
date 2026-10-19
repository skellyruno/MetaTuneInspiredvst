// Offline development renderer: mono 16-bit PCM WAV -> 32-bit float WAV.
// Renders the same engine + fixed latency compensator used by the JUCE processor.
// This is NOT a VST3 build; use FL Studio for actual host validation.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static uint16_t word(const char* b) { return (uint16_t)(uint8_t)b[0] | ((uint16_t)(uint8_t)b[1] << 8); }
static uint32_t dword(const char* b) { return (uint32_t)(uint8_t)b[0] | ((uint32_t)(uint8_t)b[1] << 8) | ((uint32_t)(uint8_t)b[2] << 16) | ((uint32_t)(uint8_t)b[3] << 24); }
static void w16(std::ofstream& out,uint16_t x) { char b[]={(char)x,(char)(x>>8)};out.write(b,2); }
static void w32(std::ofstream& out,uint32_t x) { char b[]={(char)x,(char)(x>>8),(char)(x>>16),(char)(x>>24)};out.write(b,4); }
int main(int argc,char** argv)
{
    if(argc < 3 || argc > 11) { std::cerr << "Usage: render_wav mono_pcm16.wav out_float.wav [--key 0..11] [--normal] [--live] [--bypass] [--precision] [--trace output.csv]\n"; return 2; }
    bool bypass=false, live=false, normal=false, precision=false;
    int key=0;
    std::string tracePath;
    for(int k=3;k<argc;++k)
    {
        if(std::strcmp(argv[k],"--bypass")==0) bypass=true;
        else if(std::strcmp(argv[k],"--normal")==0) normal=true;
        else if(std::strcmp(argv[k],"--precision")==0) precision=true;
        else if(std::strcmp(argv[k],"--key")==0 && k+1<argc) key=std::max(0,std::min(11,std::atoi(argv[++k])));
        else if(std::strcmp(argv[k],"--live")==0) live=true;
        else if(std::strcmp(argv[k],"--trace")==0 && k+1<argc) tracePath=argv[++k];
        else { std::cerr << "Unknown or incomplete option\n"; return 2; }
    }
    if(live && normal)
    {
        std::cerr << "--live requires the Low Latency synthesis engine (remove --normal)\n";
        return 2;
    }
    std::ofstream trace;
    if(!tracePath.empty())
    {
        trace.open(tracePath);
        if(!trace) { std::cerr << "Cannot open trace file\n"; return 12; }
        trace << "time_s,hop,detected_midi,target_midi,confidence,correction_st,"
                 "target_ratio,smoothed_ratio,period_samples,active_grains,"
                 "synthesis_gain,grain_dropouts,voiced,pending_note,"
                 "raw_f0_hz,alternate_f0_hz,phase_correlation,overlap_weight_sum,source_age_samples,phase_reacquisitions,scheduled_output_period_samples\n";
    }
    std::ifstream in(argv[1],std::ios::binary);
    char head[12]; if(!in.read(head,12) || std::memcmp(head,"RIFF",4) || std::memcmp(head+8,"WAVE",4)) return 3;
    std::vector<int16_t> pcm; uint32_t rate=0; bool hasFmt=false,hasData=false;
    while (in)
    {
        char chunk[8]; if(!in.read(chunk,8)) break;
        const uint32_t len=dword(chunk+4);
        if(len > 100000000u) return 4;
        std::vector<char> bytes(len);
        if(!in.read(bytes.data(),len)) return 5;
        if((len&1u) && !in.get()) return 6;
        if(!std::memcmp(chunk,"fmt ",4))
        {
            if(len<16 || word(bytes.data())!=1 || word(bytes.data()+2)!=1 || word(bytes.data()+14)!=16) return 7;
            rate=dword(bytes.data()+4);hasFmt=true;
        }
        if(!std::memcmp(chunk,"data",4))
        {
            if((len&1u) || pcm.size()) return 8;
            pcm.resize(len/2);
            for(size_t i=0;i<pcm.size();++i) pcm[i]=(int16_t)word(bytes.data()+2*i);
            hasData=true;
        }
    }
    if(!hasFmt || !hasData || rate<8000 || rate>192000 || pcm.empty()) return 9;
    const int delay=autotune::RealtimeAutotune::recommendedHostLatencySamples(rate,70.0f,live);
    const size_t n=pcm.size()+(size_t)delay+(size_t)(rate/10u);
    if(n>25000000u) return 10;
    std::vector<float> data(n,0.0f);
    for(size_t i=0;i<pcm.size();++i)data[i]=(float)pcm[i]/32768.0f;
    autotune::RealtimeAutotune tune;
    tune.prepare(rate);
    autotune::Settings opts;opts.key=key;opts.scale=autotune::Scale::Major;
    opts.retuneMs=0.0f;opts.follow=1.0f;opts.noteStabilizerMs=0.0f;
    opts.vibratoPreserve=1.0f;opts.voicingThreshold=0.82f;
    opts.precisionHardTune=precision;
    opts.lowLatency=!normal; // Live additionally uses the smaller, fixed-fast host budget.
    tune.setSettings(opts);
    autotune::OutputLatencyCompensator comp;
    comp.prepare(delay,rate);
    comp.setPitchPreserving(true);
    const int block=trace.is_open()?64:256;
    std::vector<float> delays((size_t)block);
    uint64_t lastTraceHop=0;
    for(size_t i=0;i<n;i+=block)
    {
        const int size=(int)std::min((size_t)block,n-i);
        float* ch[]={data.data()+i};
        if(!bypass) tune.processChannels(ch,1,size,delays.data());
        if(trace.is_open() && !bypass)
        {
            const auto d=tune.getDiagnostics();
            if(d.analysisHop!=lastTraceHop)
            {
                lastTraceHop=d.analysisHop;
                trace << ((double)(i+(size_t)size)/(double)rate) << ',' << d.analysisHop << ','
                      << d.detectedMidi << ',' << d.targetMidi << ',' << d.confidence << ','
                      << d.correctionSemitones << ',' << d.targetRatio << ',' << d.smoothedRatio << ','
                      << d.estimatedPeriodSamples << ',' << d.activeGrains << ','
                      << d.synthesisGain << ',' << d.grainDropouts << ',' << (int)d.voiced << ','
                      << (int)d.pendingNote << ',' << d.rawFrequencyHz << ','
                      << d.alternateFrequencyHz << ',' << d.phaseCorrelation << ','
                      << d.overlapWeightSum << ',' << d.sourceAgeSamples << ',' << d.phaseReacquisitions << ',' << d.scheduledOutputPeriodSamples << '\n';
            }
        }
        comp.processChannels(ch,1,size,bypass?nullptr:delays.data());
    }
    std::ofstream out(argv[2],std::ios::binary);
    const uint32_t b=(uint32_t)(n*sizeof(float));
    out.write("RIFF",4);w32(out,36+b);out.write("WAVEfmt ",8);w32(out,16);
    w16(out,3);w16(out,1);w32(out,rate);w32(out,rate*4);w16(out,4);w16(out,32);
    out.write("data",4);w32(out,b);
    out.write((char*)data.data(),b);
    if(!out) return 11;
    std::cout << "Rendered " << n << " samples at " << rate << " Hz (" << (live?"Live":"switchable")
              << " host PDC " << delay << ", over-budget telemetry "
              << comp.getOverBudgetSamples() << ")\n";
}
