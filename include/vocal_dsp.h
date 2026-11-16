#pragma once
#include <Arduino.h>
#include <SD.h>
#include <math.h>

// Standalone CPU-side DSP for mono PCM16 WAV; no browser is needed.
// Ogg Vorbis requires a separate decoder or PCM capture path.
namespace VocalDSP {
struct Wav {
  uint32_t sampleRate=0, dataOffset=0, dataBytes=0;
  uint16_t channels=0, bits=0, format=0;
};
inline uint16_t le16(const uint8_t*p){return uint16_t(p[0]) | uint16_t(p[1])<<8;}
inline uint32_t le32(const uint8_t*p){return uint32_t(le16(p)) | uint32_t(le16(p+2))<<16;}
inline bool parseWav(File &f,Wav &w) {
  uint8_t h[12];
  if(f.read(h,12)!=12 || memcmp(h,"RIFF",4) || memcmp(h+8,"WAVE",4))return false;
  while(f.available()>=8) {
    uint8_t c[8];if(f.read(c,8)!=8)return false;
    uint32_t n=le32(c+4),offset=f.position();
    if(n>f.size()-offset)return false;
    if(!memcmp(c,"fmt ",4)){
      uint8_t p[16];if(n<16||f.read(p,16)!=16)return false;
      w.format=le16(p);w.channels=le16(p+2);w.sampleRate=le32(p+4);w.bits=le16(p+14);
    } else if(!memcmp(c,"data",4)){
      w.dataOffset=offset;w.dataBytes=n;
    }
    if(!f.seek(offset+n+(n&1)))return false;
    if(w.dataOffset && w.sampleRate)break;
  }
  return w.format==1 && w.channels==1 && w.bits==16 &&
    w.sampleRate>=8000 && w.sampleRate<=48000 && w.dataBytes>=2048;
}
struct Frame {float hz=0, confidence=0, rmsDb=-120;};
inline Frame measure(const int16_t *s,size_t n,uint32_t sr){
  Frame r;double power=0;
  for(size_t i=0;i<n;i++){double v=s[i]/32768.0;power+=v*v;}
  double rms=sqrt(power/n);
  r.rmsDb=20.0f*log10f(fmaxf(rms,1e-6f));
  if(rms<0.008f)return r;
  // Normalized autocorrelation, search only voice fundamental range.
  const int lo=max(2,int(sr/1000)),hi=min(int(n/2),int(sr/75));
  float best=0;int lagBest=0;
  for(int lag=lo;lag<=hi;lag++){
    double ab=0,aa=0,bb=0;
    for(size_t i=0;i+lag<n;i++){
      double a=s[i],b=s[i+lag];ab+=a*b;aa+=a*a;bb+=b*b;
    }
    float corr=ab/sqrt(aa*bb+1e-12);
    // Prefer the first convincing local maximum to reduce octave errors.
    if(corr>best){best=corr;lagBest=lag;}
  }
  if(best>=0.78f && lagBest){r.hz=float(sr)/lagBest;r.confidence=best;}
  return r;
}
inline bool analyzeWav(File &input,File &output,String &error){
  Wav w;
  if(!parseWav(input,w)){error="Требуется mono PCM16 WAV 8-48 kHz";return false;}
  const size_t samples=2048;
  int16_t *buf=(int16_t*)malloc(samples*sizeof(int16_t));
  if(!buf){error="Недостаточно памяти";return false;}
  if(!input.seek(w.dataOffset)){free(buf);error="WAV seek error";return false;}
  // Fixed window and hop, bounded CPU and RAM. Process up to 30 minutes.
  const uint32_t hop=w.sampleRate/50;
  const uint32_t total=min(uint32_t(w.dataBytes/2),uint32_t(w.sampleRate*1800UL));
  uint32_t pos=0;bool ok=true;uint32_t frames=0;
  output.print("{\"version\":1,\"algorithm\":\"esp32-normalized-autocorrelation-v1\",\"sampleRate\":");
  output.print(w.sampleRate);
  output.print(",\"frames\":[");
  while(pos+samples<=total){
    if(input.read((uint8_t*)buf,samples*2)!=(int)(samples*2)){error="WAV read failed";ok=false;break;}
    Frame m=measure(buf,samples,w.sampleRate);
    if(frames++)output.print(',');
    output.print("{\"time\":");output.print(float(pos+samples/2)/w.sampleRate,3);
    output.print(",\"hz\":");if(m.hz)output.print(m.hz,2);else output.print("null");
    output.print(",\"confidence\":");output.print(m.confidence,3);
    output.print(",\"rmsDb\":");output.print(m.rmsDb,2);output.print('}');
    if(!output){error="SD write failed";ok=false;break;}
    pos+=hop;
    if(!input.seek(w.dataOffset+pos*2)){error="WAV seek failed";ok=false;break;}
    if(frames%10==0)yield();
  }
  output.print("]}");
  output.flush();
  free(buf);
  return ok && frames>0 && bool(output);
}
} // namespace VocalDSP
