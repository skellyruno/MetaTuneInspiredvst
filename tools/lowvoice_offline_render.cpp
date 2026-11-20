// Private-vocal evaluation utility: the WAVs remain on the local machine.
// Compile from the same portable Lyra sources as the VST3, with the LAB
// macro for the experimental renderer. The utility itself has NO private
// recording embedded in the source tree or GitHub Actions artifact.
#include "TunerChain.h"
#include "Pitch/Scale.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {
uint16_t u16(const std::vector<unsigned char>& b,size_t p) {
    return uint16_t(b[p]) | uint16_t(uint16_t(b[p+1]) << 8);
}
uint32_t u32(const std::vector<unsigned char>& b,size_t p) {
    return uint32_t(b[p]) | (uint32_t(b[p+1])<<8) |
        (uint32_t(b[p+2])<<16) | (uint32_t(b[p+3])<<24);
}
void put16(std::ofstream& o,uint16_t v) {
    const unsigned char b[2]{(unsigned char)v,(unsigned char)(v>>8)};
    o.write((const char*)b,2);
}
void put32(std::ofstream& o,uint32_t v) {
    const unsigned char b[4]{(unsigned char)v,(unsigned char)(v>>8),
        (unsigned char)(v>>16),(unsigned char)(v>>24)};
    o.write((const char*)b,4);
}
struct Audio {
    uint32_t rate=0;
    uint16_t channels=0;
    std::vector<float> samples;
};
bool readWav(const std::string& path,Audio& dst)
{
    std::ifstream f(path,std::ios::binary);
    if(!f) return false;
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
    if(b.size()<44 || std::memcmp(b.data(),"RIFF",4) ||
       std::memcmp(b.data()+8,"WAVE",4)) return false;
    uint16_t fmt=0,bits=0;
    size_t off=12, data=0, bytes=0;
    while(off+8<=b.size()) {
        const size_t size=u32(b,off+4);
        if(size>b.size()-off-8) return false;
        if(!std::memcmp(b.data()+off,"fmt ",4) && size>=16) {
            fmt=u16(b,off+8); dst.channels=u16(b,off+10);
            dst.rate=u32(b,off+12); bits=u16(b,off+22);
        }
        if(!std::memcmp(b.data()+off,"data",4)) {data=off+8; bytes=size;}
        off+=8+size+(size&1u);
    }
    if(!(fmt==1||fmt==3) || !(bits==16||bits==24||bits==32) ||
       (fmt==3&&bits!=32) || dst.channels<1 || dst.channels>2 ||
       dst.rate<8000 || dst.rate>192000 || bytes==0) return false;
    const size_t bps=bits/8, frames=bytes/(bps*dst.channels);
    if(frames>size_t(dst.rate)*120) return false;
    dst.samples.resize(frames*dst.channels);
    for(size_t i=0;i<dst.samples.size();++i) {
        const size_t j=data+i*bps;
        float v=0.0f;
        if(fmt==3) {
            const uint32_t word=u32(b,j);
            std::memcpy(&v,&word,4);
        } else if(bits==16) {
            v=float((int16_t)u16(b,j))/32768.0f;
        } else if(bits==24) {
            int32_t word=int32_t(uint32_t(b[j])|(uint32_t(b[j+1])<<8)|
                                (uint32_t(b[j+2])<<16));
            if(word&0x800000)word|=~0xFFFFFF;
            v=float(word)/8388608.0f;
        } else {
            v=float((int32_t)u32(b,j))/2147483648.0f;
        }
        dst.samples[i]=std::isfinite(v)?std::clamp(v,-8.0f,8.0f):0.0f;
    }
    return true;
}
bool writeWav(const std::string& path,const Audio& a)
{
    std::ofstream o(path,std::ios::binary);
    if(!o || a.samples.size()>std::numeric_limits<uint32_t>::max()/4) return false;
    const auto n=(uint32_t)(a.samples.size()*4);
    o.write("RIFF",4);put32(o,n+36);o.write("WAVE",4);
    o.write("fmt ",4);put32(o,16);put16(o,3);put16(o,a.channels);
    put32(o,a.rate);put32(o,a.rate*a.channels*4);
    put16(o,a.channels*4);put16(o,32);
    o.write("data",4);put32(o,n);
    o.write((const char*)a.samples.data(),n);
    return o.good();
}
} // namespace
int main(int argc,char** argv)
{
    if(argc!=5 && argc!=6) {
        std::fprintf(stderr,"Usage: lowvoice_offline_render input.wav output.wav Eminor|Bmajor lab|classic|quality\n");
        return 2;
    }
    Audio a;
    if(!readWav(argv[1],a)) {
        std::fprintf(stderr,"Only 1-2ch RIFF PCM16/24/32 and IEEE float32 WAV <=120s are supported\n");
        return 3;
    }
    const std::string key=argv[3],mode=argv[4];
    if((key!="Eminor" && key!="Bmajor") ||
       (mode!="lab" && mode!="classic" && mode!="quality")) return 4;
    lyra::TunerChain chain;
    chain.prepare(a.rate,70.0);
    lyra::TunerChain::Settings s;
    s.minFreq=70.0;
    s.ultraLive=mode!="quality";
    s.hq=mode=="quality";
    s.experimentalLowVoice=mode=="lab";
    s.correction.mask=lyra::scaleMask(key=="Eminor"
                                    ? lyra::ScaleType::Minor
                                    : lyra::ScaleType::Major,
                                    key=="Eminor"?4:11);
    s.correction.minConfidence=0.65f;
    s.correction.skellLive=mode!="quality";
    s.correction.hysteresisCents=7.0;
    s.correction.stabilizerMs=0.0;
    s.correction.speedMs=-3.0;
    s.correction.amount=1.0;
    s.correction.glideMs=0.0;
    chain.setSettings(s);
    chain.reset();
    // Optional PRIVATE pitch trace; neither the WAVs nor their telemetry
    // ever appear in GitHub source or its CI build artifact.
    lyra::TelemetryQueue telemetry;
    std::ofstream trace;
    if(argc==6)
    {
        trace.open(argv[5]);
        if(!trace) return 7;
        trace<<"time_s,input_midi,output_midi,confidence,rms,target,pending\n";
        chain.setTelemetryQueue(&telemetry);
    }
    const int block=128;
    const size_t frames=a.samples.size()/a.channels;
    float channels[2][block]{};
    float peak=0.0f, maxStep=0.0f, prev=0.0f;
    const auto begin=std::chrono::steady_clock::now();
    std::vector<double> blockCpu;
    blockCpu.reserve((frames+block-1)/block);
    for(size_t start=0;start<frames;start+=block) {
        const int n=int(std::min(size_t(block),frames-start));
        for(int i=0;i<n;++i)
            for(int c=0;c<a.channels;++c)
                channels[c][i]=a.samples[(start+i)*a.channels+c];
        float* ch[]{channels[0],channels[1]};
        const auto blockBegin=std::chrono::steady_clock::now();
        chain.process(ch,a.channels,n);
        const auto blockEnd=std::chrono::steady_clock::now();
        blockCpu.push_back(std::chrono::duration<double>(blockEnd-blockBegin).count()
                           / (double(n)/a.rate));
        if(trace)
        {
            lyra::Telemetry event;
            while(telemetry.pop(event))
                trace<<((double(start)+n)/a.rate)<<","<<event.inputMidi<<","
                     <<event.outputMidi<<","<<event.confidence<<","
                     <<event.rms<<","<<event.target<<","<<event.pending<<"\n";
        }
        for(int i=0;i<n;++i)
            for(int c=0;c<a.channels;++c) {
                const float v=channels[c][i];
                if(!std::isfinite(v)) {
                    std::fprintf(stderr,"Non-finite DSP output!\n");
                    return 5;
                }
                peak=std::max(peak,std::abs(v));
                if(c==0 && start+i>1024) {
                    maxStep=std::max(maxStep,std::abs(v-prev));
                }
                if(c==0)prev=v;
                a.samples[(start+i)*a.channels+c]=v;
            }
    }
    const auto end=std::chrono::steady_clock::now();
    const double cpu=std::chrono::duration<double>(end-begin).count() /
                     (double(frames)/a.rate);
    if(!writeWav(argv[2],a)) return 6;
    std::sort(blockCpu.begin(),blockCpu.end());
    const auto percentile=[&](double fraction)->double {
        if(blockCpu.empty())return 0.0;
        return blockCpu[std::min(blockCpu.size()-1,
            size_t(fraction*double(blockCpu.size()-1)))];
    };
    std::printf("block_cpu_fraction_p95=%.4f p99=%.4f max=%.4f (Linux runner, not user's hardware)\n",
                percentile(0.95),percentile(0.99),percentile(1.0));
    std::printf("mode=%s input=%s duration=%.3fs sr=%uHz channels=%u\n",
        mode.c_str(),argv[1],double(frames)/a.rate,a.rate,a.channels);
    std::printf("nominal_PDC=%d samples / %.3fms, measured_process_fraction=%.4f, peak=%.5f, max_step=%.5f\n",
        chain.latencySamples(),1000.0*chain.latencySamples()/a.rate,cpu,peak,maxStep);
#if defined(SKELLTUNE_LOW_VOICE_LAB) && SKELLTUNE_LOW_VOICE_LAB
    if(mode=="lab")
        std::printf("lab_splices=%d, low_voice_source_age_min=%.0f,max=%.0f samples\n",
            chain.lowVoiceLabSplices(),chain.lowVoiceLabMinSourceAge(),
            chain.lowVoiceLabMaxSourceAge());
#endif
    return 0;
}
