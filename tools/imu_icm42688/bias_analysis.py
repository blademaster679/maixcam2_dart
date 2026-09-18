"""Static retest analysis: discard warmup and freeze bias before holdout.

The caller must independently confirm stationary conditions. Gravity cannot be
removed from an unknown single accelerometer pose by averaging.
"""
import argparse
import json
import math
from pathlib import Path
from analyze import pool, validate_raw


def select(windows, begin, end):
    # Only complete windows fully inside the interval; do not mix warmup samples.
    return [w for w in windows if w['n'] and w['end_s']-w['duration_s'] >= begin
            and w['end_s'] <= end]


def slope(points):
    if len(points) < 2:
        return None
    xbar = sum(x for x, y in points) / len(points)
    ybar = sum(y for x, y in points) / len(points)
    den = sum((x-xbar)**2 for x, y in points)
    return sum((x-xbar)*(y-ybar) for x, y in points)/den if den else None


def analyze_bias(summary, stationary_confirmed=False):
    phase = summary['phases'][0]
    windows = phase['windows']
    end = phase['elapsed_s']
    warmup = pool(select(windows, 0, 600))
    measurement = pool(select(windows, 600, end))
    train = pool(select(windows, 600, 1200))
    holdout = pool(select(windows, 1200, end))
    if not measurement['n'] or not train['n'] or not holdout['n']:
        raise ValueError('Insufficient post-warmup data for separate train/holdout')
    minutes = []
    for t in range(0, math.floor(end), 60):
        group = [w for w in windows if w['n'] and t <= w['end_s']-w['duration_s']/2 < t+60]
        if group:
            minutes.append(dict(begin_s=t, end_s=min(t+60, end), **pool(group)))
    # The previous minute is needed to assess all five one-minute changes.
    six = [m for m in minutes if 240 <= m['begin_s'] < 600]
    changes = [b['mean'][6]-a['mean'][6] for a, b in zip(six, six[1:])]
    post = select(windows, 600, end)
    result = dict(
        stationary_confirmed_by_user=stationary_confirmed, nominal_odr_hz=1000, warmup_excluded_s=600,
        measurement=measurement, warmup=warmup,
        calibration_train=train, calibration_holdout=holdout,
        calibration_note='Offline subtraction only; no offset registers written.',
        gyro_holdout_residual_dps=[holdout['mean'][i]-train['mean'][i] for i in range(3, 6)],
        accel_mean_vector_norm_g=math.sqrt(sum(x*x for x in measurement['mean'][:3])),
        temperature_range_post_warmup_C=[min(w['min'][6] for w in post),max(w['max'][6] for w in post)],
        temperature_slope_post_warmup_C_per_min=slope([(w['end_s']/60,w['mean'][6]) for w in post]),
        pre_measurement_five_minute_temperature_changes_C=changes,
        warmup_stability_criterion_met=len(changes)==5 and all(abs(c)<.2 for c in changes),
        gyro_mean_slopes_dps_per_min=[slope([(w['end_s']/60,w['mean'][i]) for w in post]) for i in range(3,6)],
        minute_windows=minutes,
        gyro_10s_mean_range_post_warmup_dps=[[min(w['mean'][i] for w in post),max(w['mean'][i] for w in post)] for i in range(3,6)],
        header_errors=phase['bad_headers'], invalid_packets=phase['invalid_packets'],
        fifo_full_observations=phase['fifo_full_observations'],
        total_samples=phase['stats']['n'], elapsed_s=end, rate_hz=phase['rate_hz'],
        restored_registers_match=summary['original_registers']==summary['restored_registers'],
        restored_pins_match=summary['original_pins']==summary['restored_pins'],
        cleanup_errors=summary['cleanup_errors'])
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('summary', type=Path)
    parser.add_argument('--validate-raw', action='store_true')
    parser.add_argument('--stationary-confirmed', action='store_true')
    args = parser.parse_args()
    result = analyze_bias(json.loads(args.summary.read_text()), args.stationary_confirmed)
    if args.validate_raw:
        result['raw_validation'] = validate_raw(args.summary.parent/'static_1k.bin')
    print(json.dumps(result, indent=2))
