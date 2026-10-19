// Regression from a 0.9-second excerpt of the user's own raw vocal.
// The voice stays near 200 Hz while earlier double-lag YIN logic snapped
// between ~200 and ~100 Hz despite extremely high reported confidence.
// This tests DETECTION, not perceptual tuning or correct key selection.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <vector>

static uint16_t u16(const unsigned char* p) {return (uint16_t)(p[0] | (p[1]<<8));}
static uint32_t u32(const unsigned char* p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int main(int argc,char** argv)
{
    if(argc!=2) return 10;
    std::ifstream in(argv[1],std::ios::binary);
    unsigned char header[44];
    if (!in.read((char*)header,44) || std::string((char*)header,4)!="RIFF"
        || std::string((char*)header+8,4)!="WAVE" || u16(header+20)!=1
        || u16(header+22)!=1 || u32(header+24)!=44100 || u16(header+34)!=16
        || std::string((char*)header+36,4)!="data") return 11;
    const size_t n=u32(header+40)/2;
    if(n<30000 || n>55000) return 12;
    std::vector<unsigned char> bytes(n*2);
    if(!in.read((char*)bytes.data(),(std::streamsize)bytes.size()))return 13;
    autotune::YinDetector detector;
    detector.prepare(44100.0,70.0f,1000.0f);
    int voiced=0,octave=0,accurate=0;
    for(size_t i=0;i<n;++i)
    {
        const int16_t val=(int16_t)u16(bytes.data()+i*2);
        if (!detector.push((float)val/32768.0f))continue;
        const float secs=(float)i/44100.0f;
        if(secs<.30f||secs>.62f || detector.getConfidence()<.90f)continue;
        const float f=detector.getFrequency();
        ++voiced;
        if(f>180.0f && f<235.0f)++accurate;
        if(f>86.0f && f<119.0f)++octave;
    }
    std::printf("Real voice 200-Hz octave regression: voiced=%d accurate=%d octave-down=%d\n",voiced,accurate,octave);
    return voiced>=60 && accurate>=voiced*0.85 && octave<=voiced*0.03 ? 0:1;
}
