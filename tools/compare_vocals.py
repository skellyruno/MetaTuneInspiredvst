#!/usr/bin/env python3
"""Reproducible REAL-vocal reference comparison with envelope-based timeline alignment.

Dependencies: numpy scipy soundfile librosa. Input recordings are private; this
script neither uploads them nor labels F0 differences as perceptual sound quality.

Example:
 python tools/compare_vocal_aligned_v13.py --raw 'raw vocals.mp3' \
   --reference 'autotune pro Vocals (a major).mp3' \
   --render tuned.wav --trace tuned.csv --name 'SkellTune V13' --output report.csv

Comparison shift is estimated from *amplitude envelopes*, not pitch closeness,
so a pitch algorithm cannot improve its score just by moving the search window.
"""
import argparse
import csv
import numpy as np
import librosa
from scipy.signal import correlate, correlation_lags
from scipy.ndimage import gaussian_filter1d

SAMPLE_RATE=22050
HOP=256
FRAME=2048
MAJOR_A={1,2,4,6,8,9,11}

def load(path):
    y,_=librosa.load(path,sr=SAMPLE_RATE,mono=True)
    if len(y)<SAMPLE_RATE: raise ValueError('Need at least 1 second of audio')
    return y

def extract(y):
    f0=librosa.yin(y, fmin=70,fmax=650,sr=SAMPLE_RATE,frame_length=FRAME,hop_length=HOP)
    rms=librosa.feature.rms(y=y,frame_length=FRAME,hop_length=HOP)[0]
    flat=librosa.feature.spectral_flatness(y=y,n_fft=FRAME,hop_length=HOP)[0]
    return {'pitch':librosa.hz_to_midi(f0), 'rms':rms, 'flat':flat}

def time_align(reference_rms, output_rms, max_ms=110.0):
    # Long-window RMS includes meaningful syllable onsets. Compress dynamic
    # range and remove gradual level changes so timing wins over volume.
    a=np.log(np.maximum(reference_rms,1e-4))
    b=np.log(np.maximum(output_rms,1e-4))
    a=gaussian_filter1d(a,1)-gaussian_filter1d(a,25)
    b=gaussian_filter1d(b,1)-gaussian_filter1d(b,25)
    corr=correlate(b, a,mode='full',method='fft')
    lags=correlation_lags(len(b),len(a),mode='full')
    max_frames=int(round(max_ms * 0.001 * SAMPLE_RATE / HOP))
    mask=(lags>=-max_frames)&(lags<=max_frames)
    lag=int(lags[mask][np.argmax(corr[mask])])
    return lag

def align(data,lag,n):
    idx=np.arange(n)+lag
    valid=(idx>=0)&(idx<len(data))
    return data[np.clip(idx,0,len(data)-1)], valid

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for k in ['raw','reference','render']:ap.add_argument('--'+k,required=True)
    ap.add_argument('--trace',default='')
    ap.add_argument('--name',default='SkellTune')
    ap.add_argument('--output',default='')
    args=ap.parse_args()
    raw=extract(load(args.raw));ref=extract(load(args.reference));out=extract(load(args.render));
    count=min(len(raw['pitch']),len(ref['pitch']))
    lag=time_align(ref['rms'][:count],out['rms'])
    target,lv=align(out['pitch'],lag,count)
    r=raw['pitch'][:count];m=ref['pitch'][:count]
    mask=(raw['rms'][:count]>.075)&(ref['rms'][:count]>.075)&(raw['flat'][:count]<.08)
    mask &= np.abs(r-m)<6.6
    mask &= lv
    allowed=np.asarray([n for n in range(20,110) if n%12 in MAJOR_A])
    error_ref=100*np.min(np.abs(m[:,None]-allowed[None,:]),axis=1)
    error_raw=100*np.min(np.abs(r[:,None]-allowed[None,:]),axis=1)
    error_out=100*np.min(np.abs(target[:,None]-allowed[None,:]),axis=1)
    diff=100*np.abs(target-m)
    row={ 'render':args.name,
         'envelope_lag_ms':round(lag*1000*HOP/SAMPLE_RATE,3),
         'valid_frames':int(mask.sum()),
         'raw_A_major_median_cents':round(float(np.median(error_raw[mask])),2),
         'reference_A_major_median_cents':round(float(np.median(error_ref[mask])),2),
         'render_A_major_median_cents':round(float(np.median(error_out[mask])),2),
         'render_reference_median_cents':round(float(np.median(diff[mask])),2),
         'render_reference_p90_cents':round(float(np.percentile(diff[mask],90)),2)}
    if args.trace:
        with open(args.trace,newline='') as f: rows=list(csv.DictReader(f))
        t=np.array([float(x['time_s']) for x in rows])
        note=np.array([float(x['target_midi']) for x in rows])
        conf=np.array([float(x['confidence']) for x in rows])
        # The renderer's trace time is the input time, whereas output has
        # host latency. Envelope alignment makes the two clocks comparable.
        timeline=np.arange(count)*HOP/SAMPLE_RATE
        notes=np.interp(timeline,t,note,left=-1,right=-1)
        certainty=np.interp(timeline,t,conf,left=0,right=0)
        locked=mask&(certainty>.85)&(notes>=0)&(np.abs(notes-m)<.2)
        if locked.sum()>10:
            row['frames_target_agrees_reference']=int(locked.sum())
            row['render_ref_median_when_target_matches_cents']=round(float(np.median(diff[locked])),2)
    for k,v in row.items(): print(f'{k}: {v}')
    if args.output:
        with open(args.output,'w',newline='') as f:
            w=csv.DictWriter(f,fieldnames=list(row));w.writeheader();w.writerow(row)
if __name__=='__main__':main()
