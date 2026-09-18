"""Held-out, causal replay of a recorded 20-minute stationary IMU test.

The first 300 s warm up, 300..600 s train, and 600..1200 s validate.
Complete one-second angle increments are integrals, not sample-noise RMS.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import hashlib
import numpy as np
from drift_filter import RestBias, Settings, lowpass


PACKET = np.dtype([('header','u1'), ('accel','>i2',(3,)), ('gyro','>i2',(3,)),
                   ('temperature','i1'), ('timestamp','>u2')])


def one_second_angles(values, dt, seconds):
    # Every raw sample is a zero-order-held value on [i*dt, (i+1)*dt).
    # Interpolate the cumulative integral, so fractional boundary samples are
    # correctly apportioned and constant 1 deg/s gives exactly 1 degree/second.
    cumulative = np.vstack([np.zeros(3), np.cumsum(values, axis=0)*dt])
    position = np.arange(seconds+1)/dt
    index = np.minimum(position.astype(np.int64), len(values)-1)
    frac = position-index
    integrals = cumulative[index] + frac[:, None]*values[index]*dt
    return np.diff(integrals, axis=0)


def metrics(angles):
    cumulative = np.cumsum(angles, axis=0)
    return dict(seconds=len(angles), mean_signed_deg_per_s=angles.mean(axis=0).tolist(),
                mean_abs_one_second_deg=np.abs(angles).mean(axis=0).tolist(),
                rms_one_second_deg=np.sqrt((angles**2).mean(axis=0)).tolist(),
                p95_abs_one_second_deg=np.percentile(np.abs(angles),95,axis=0).tolist(),
                max_abs_one_second_deg=np.abs(angles).max(axis=0).tolist(),
                net_integrated_axis_deg=cumulative[-1].tolist(),
                max_abs_cumulative_axis_deg=np.abs(cumulative).max(axis=0).tolist(),
                mean_one_second_vector_norm_deg=float(np.linalg.norm(angles,axis=1).mean()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('run', type=Path)
    parser.add_argument('--confirmed-static', action='store_true')
    args = parser.parse_args()
    summary = json.loads((args.run/'summary.json').read_text())
    phase = summary['phases'][0]
    if not summary.get('complete') or any(phase[k] for k in
            ['bad_headers','invalid_packets','fifo_full_observations','reset_observations','id_errors']):
        raise ValueError('capture incomplete or unhealthy; cannot evaluate clean continuity')
    path = args.run/'static_1k.bin'
    if path.stat().st_size % 16:
        raise ValueError('partial packet at end of file')
    raw = np.memmap(path,dtype=PACKET,mode='r')
    assert len(raw)==phase['stats']['n']
    assert np.all((raw['header'] & 0xFC)==0x68)
    assert not np.any(raw['accel']==-32768) and not np.any(raw['gyro']==-32768)
    delta=(np.diff(raw['timestamp'].astype(np.int32)) & 65535)
    assert np.all((delta==937)|(delta==938)), 'discontinuous hardware timestamps'
    gyro=raw['gyro'].astype(float)/131.0
    accel=raw['accel'].astype(float)/16384.0
    dt=phase['elapsed_s']/len(raw)
    seconds=int(phase['elapsed_s'])
    assert seconds>=1200
    angles=one_second_angles(gyro,dt,seconds)
    initial=angles[300:600].mean(axis=0)
    robust=np.median(angles[300:600],axis=0)
    c=Settings()
    output=dict(protocol=dict(warmup_s=[0,300],train_s=[300,600],validation_s=[600,1200],
                              confirmed_static=args.confirmed_static,settings=asdict(c),
                              timing='Uniform sample grid anchored to measured host-monotonic duration; FIFO timestamp continuity verified.',
                              caution='Sensor-body-axis integrals; not Euler-angle errors or an external attitude-reference comparison.'),
                sample_count=len(raw),duration_s=phase['elapsed_s'],rate_hz=1/dt,
                mean_bias_dps=initial.tolist(),robust_bias_dps=robust.tolist(),
                temperature_mean_windows_C=[w['mean'][6] for w in phase['windows']],
                temperature_window_ends_s=[w['end_s'] for w in phase['windows']],
                raw_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),methods={})
    def record(name, values):
        a=one_second_angles(values,dt,seconds)[600:1200]
        st=int(np.ceil(600/dt));en=int(np.floor(1200/dt))
        output['methods'][name]=dict(**metrics(a),
             sample_rms_dps=np.sqrt((values[st:en]**2).mean(axis=0)).tolist(),
             one_second_axis_deg=a.tolist())
    record('raw',gyro)
    record('mean_bias',gyro-initial)
    record('robust_bias',gyro-robust)
    corrected=gyro-robust
    record('robust_bias_lowpass',lowpass(corrected,dt,c.cutoff_hz))
    tracker=RestBias(robust,c)
    history=[]
    for sec in range(600,1200):
        start=int(np.ceil(sec/dt));end=min(len(raw),int(np.ceil((sec+1)/dt)))
        before=tracker.bias.copy()
        corrected[start:end]=tracker.correct(gyro[start:end])
        status=tracker.finish_window(gyro[start:end],accel[start:end],1,
                                     confirmed_static=args.confirmed_static)
        history.append(dict(window_end_s=sec+1,bias_used_dps=before.tolist(),**status))
    record('rest_adaptive_lowpass',lowpass(corrected,dt,c.cutoff_hz))
    output['adaptive_history']=history
    output['adaptive_updated_seconds']=sum(x['updated'] for x in history)
    output['restored_registers_match']=summary['original_registers']==summary['restored_registers']
    output['restored_pins_match']=summary['original_pins']==summary['restored_pins']
    target=args.run/'optimization.json'
    target.with_suffix('.tmp').write_text(json.dumps(output,separators=(',',':')))
    target.with_suffix('.tmp').replace(target)
    print(json.dumps({k:v for k,v in output.items() if k not in ['methods','adaptive_history']}))
    for name,m in output['methods'].items():
        print(name,json.dumps({k:v for k,v in m.items() if k!='one_second_axis_deg'}))


if __name__=='__main__':
    main()
