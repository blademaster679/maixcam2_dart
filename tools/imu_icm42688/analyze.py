"""Summarize saved ten-second windows without assuming samples are motionless."""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import struct


def pool(windows):
    windows = [w for w in windows if w['n']]
    n = sum(w['n'] for w in windows)
    if not n:
        return {'n': 0}
    mean = [sum(w['n'] * w['mean'][i] for w in windows) / n for i in range(7)]
    std = [math.sqrt(sum((w['n'] - 1) * w['std'][i] ** 2 +
                        w['n'] * (w['mean'][i] - mean[i]) ** 2 for w in windows) /
                     max(1, n - 1)) for i in range(7)]
    return dict(n=n, mean=mean, std=std)


def analyze(d):
    result = {k: v for k, v in d.items() if k != 'phases'}
    phases = []
    for phase in d.get('phases', []):
        p = {k: v for k, v in phase.items() if k != 'windows'}
        windows = phase['windows']
        p['minutes'] = []
        for end in range(60, math.ceil(phase['elapsed_s'] / 60) * 60 + 1, 60):
            # The window end can overrun a nominal boundary by scheduling jitter.
            group = [w for w in windows if end-60 <= w['end_s'] - w['duration_s']/2 < end]
            if group:
                p['minutes'].append(dict(end_s=end, **pool(group)))
        p['last_60s'] = pool([w for w in windows if w['end_s']-w['duration_s'] >= phase['elapsed_s']-61])
        if phase['elapsed_s'] >= 590:
            training = pool([w for w in windows if 300 <= w['end_s']-w['duration_s']/2 < 420])
            holdout = pool([w for w in windows if w['end_s']-w['duration_s']/2 >= 420])
            p['exploratory_calibration'] = dict(
                note='Unconfirmed stationary condition; 5 min warmup only. Not final calibration acceptance.',
                training_300_to_420s=training, holdout_420s_to_end=holdout,
                gyro_holdout_residual_dps=[holdout['mean'][i]-training['mean'][i] for i in range(3, 6)])
        if p.get('last_60s', {}).get('n'):
            p['last_60s']['accel_mean_norm_g'] = math.sqrt(sum(x*x for x in p['last_60s']['mean'][:3]))
        hist = {int(k): v for k, v in phase.get('timestamp_delta_hist', {}).items()}
        expected = {937, 938} if phase['odr_code'] == 6 else {234, 235}
        p['timestamp_intervals_outside_nominal_pair'] = sum(v for k, v in hist.items() if k not in expected)
        phases.append(p)
    result['phases'] = phases
    return result


def validate_raw(path):
    """Independent raw-stream check; full-scale hits are not SPI error counts."""
    digest = hashlib.sha256()
    headers, deltas = collections.Counter(), collections.Counter()
    n = invalid = malformed = 0
    rails = [0] * 6
    prev = None
    with path.open('rb') as f:
        while True:
            block = f.read(16 * 4096)
            if not block:
                break
            digest.update(block)
            if len(block) % 16:
                raise ValueError('raw file has incomplete packet')
            for h, *values in struct.iter_unpack('>B6hbH', block):
                headers[h] += 1
                if h & 0xFC != 0x68:
                    malformed += 1
                    continue
                axes, temp, ts = values[:6], values[6], values[7]
                if -32768 in axes or temp == -128:
                    invalid += 1
                    continue
                n += 1
                for i, value in enumerate(axes):
                    rails[i] += value in (-32767, 32767)
                if prev is not None:
                    deltas[(ts-prev) & 65535] += 1
                prev = ts
    return dict(file=path.name, bytes=path.stat().st_size, sha256=digest.hexdigest(),
                valid_packets=n, invalid_packets=invalid, malformed_packets=malformed,
                header_hist=dict(headers), timestamp_delta_hist=dict(deltas),
                full_scale_rail_hits_ax_ay_az_gx_gy_gz=rails)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('summary', type=Path)
    parser.add_argument('--validate-raw', action='store_true')
    args = parser.parse_args()
    result = analyze(json.loads(args.summary.read_text()))
    if args.validate_raw:
        result['raw_validation'] = [validate_raw(p) for p in sorted(args.summary.parent.glob('*.bin'))]
    print(json.dumps(result, indent=2))
