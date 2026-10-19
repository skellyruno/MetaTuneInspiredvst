#!/usr/bin/env python3
"""Level-matched diagnostic spectral envelope of dry and wet WAVs.

Synthetic vowel example (when --generate is used) is NOT a real vocal.
For actual validation, supply a sung dry.wav, render_wav to wet.wav and run
this script on their WAV pair. Does not change any shipped presets.
"""
import argparse
from pathlib import Path
import numpy as np
from scipy import signal
from scipy.ndimage import gaussian_filter1d
import soundfile as sf


def synthesized_vowel(rate=48000, seconds=1.8, hz=233.08):
    count=int(rate*seconds)
    impulse=np.zeros(count)
    phase=0.0
    for i in range(count):
        phase+=hz/rate
        if phase>=1.0:
            phase-=1.0
            impulse[i]=1.0
    # Source/filter synthesis with distinct vowel-like F1/F2/F3 peaks.
    response=np.zeros_like(impulse)
    for centre,width,gain in ((720,105,0.85),(1280,135,0.8),(2600,180,0.56)):
        r=np.exp(-np.pi*width/rate)
        a=[1.,-2.*r*np.cos(2.*np.pi*centre/rate),r*r]
        y=signal.lfilter([1.],a,impulse)
        response+=gain*y/np.max(np.abs(y)+1e-12)
    # Gradual onset/offset to avoid falsely penalizing transition artifacts.
    env=np.clip(np.arange(count)/(rate*.065),0,1)*np.clip((count-np.arange(count))/(rate*.070),0,1)
    response=response*env
    return (0.30*response / max(1e-12,np.max(np.abs(response)))).astype('float32')


def analyse(dry_path,wet_path):
    dry,sr=sf.read(dry_path,always_2d=True,dtype='float64')
    wet,sw=sf.read(wet_path,always_2d=True,dtype='float64')
    if sr!=sw: raise ValueError('Both files must have the same sample rate')
    dry=np.mean(dry,axis=1);wet=np.mean(wet,axis=1)
    # Use common stable interval after pitch-tracker initialization and transport latency.
    i=int(.35*sr); j=min(len(dry),len(wet),int(1.5*sr))
    if j-i<sr//4: raise ValueError('Provide at least one second of vocal')
    a=dry[i:j]; b=wet[i:j]
    ra=np.sqrt(np.mean(a*a));rb=np.sqrt(np.mean(b*b))
    if min(ra,rb)<1e-6: raise ValueError('One recording is silent')
    level_delta=20*np.log10(rb/ra)
    b=b*ra/rb
    def envelope(x):
        nperseg=min(8192,len(x))
        hz,px=signal.welch(x,sr,nperseg=nperseg,noverlap=nperseg//2)
        # Smooth harmonic spikes at the same frequency resolution for both files.
        sigma=110.0/max(hz[1]-hz[0],1.0)
        return hz,gaussian_filter1d(10*np.log10(np.maximum(px,1e-16)),sigma=sigma)
    freqs,da=envelope(a);_,db=envelope(b)
    positions=[]
    for low,high in ((450,960),(970,1760),(2150,3050)):
        band=(freqs>=low)&(freqs<=high)
        if not np.any(band): raise ValueError('Sample rate too low')
        va=freqs[band][int(np.argmax(da[band]))]
        vb=freqs[band][int(np.argmax(db[band]))]
        positions.append((float(va),float(vb),float(vb-va)))
    region=(freqs>=300)&(freqs<=3200)
    diff=db[region]-da[region]
    # Spectral envelope shape error after removing overall decibel offset.
    # This is a diagnostic and not an objective measure of perceptual quality.
    shape=float(np.sqrt(np.mean((diff-np.mean(diff))**2)))
    return sr,round(float(level_delta),3),positions,round(shape,3)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--dry',type=Path,required=True)
    ap.add_argument('--wet',type=Path,required=True)
    ap.add_argument('--generate',action='store_true',help='Create SYNTHETIC vowel dry WAV before comparison')
    ap.add_argument('--out',type=Path,help='Optional CSV report')
    args=ap.parse_args()
    if args.generate:
        args.dry.parent.mkdir(parents=True,exist_ok=True)
        sf.write(args.dry,synthesized_vowel(),48000,subtype='PCM_16')
        print('Synthetic vowel generated:',args.dry)
        if not args.wet.exists():
            print('Now render it: render_wav',args.dry,args.wet)
            return
    sr,level,formants,shape=analyse(args.dry,args.wet)
    print(f'sample_rate={sr}; measured output/dry original level={level:+.2f} dB; internally level-matched')
    for i,(dry,wet,delta) in enumerate(formants,1):
        print(f'F{i} dry/wet/shift = {dry:.0f}/{wet:.0f}/{delta:+.0f} Hz')
    print(f'Level-matched smoothed-envelope shape RMS difference: {shape:.2f} dB')
    if args.out:
        args.out.parent.mkdir(parents=True,exist_ok=True)
        args.out.write_text('metric,value\n'+'\n'.join([
            f'sample_rate,{sr}',f'output_vs_dry_level_db,{level}',
            *(f'f{i}_shift_hz,{v[2]:.3f}' for i,v in enumerate(formants,1)),
            f'level_matched_envelope_shape_rms_db,{shape}'])+'\n')

if __name__=='__main__': main()
