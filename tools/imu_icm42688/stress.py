"""Bounded ICM-42688-P SPI/FIFO test. Run on MaixCAM2; preserve raw packets.

No bias correction is applied. A stationary fixture is required for gyro bias;
one pose cannot identify all three accelerometer offsets.
"""
import argparse
import collections
import json
import math
import os
from pathlib import Path
import signal
import struct
import time
import traceback

from spi import SPI


class Stats:
    def __init__(self):
        self.n = 0
        self.mean = [0.] * 7
        self.m2 = [0.] * 7
        self.lo = [float('inf')] * 7
        self.hi = [float('-inf')] * 7

    def add(self, values):
        self.n += 1
        for i, x in enumerate(values):
            d = x - self.mean[i]
            self.mean[i] += d / self.n
            self.m2[i] += d * (x - self.mean[i])
            self.lo[i] = min(self.lo[i], x)
            self.hi[i] = max(self.hi[i], x)

    def result(self):
        return dict(n=self.n, axes='ax_g ay_g az_g gx_dps gy_dps gz_dps temp_C'.split(),
                    mean=self.mean, std=[math.sqrt(x / max(1, self.n - 1)) for x in self.m2],
                    min=self.lo if self.n else [], max=self.hi if self.n else [])


def save(path, data):
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(data, indent=2))
    tmp.replace(path)


def phase(s, out, name, seconds, odr, speed):
    s.speed = speed
    s.write(0x16, 0)
    s.write(0x4E, 0)
    time.sleep(.05)
    for reg, value in [(0x4F, 0x60 | odr), (0x50, 0x60 | odr),
                       (0x52, 0x44), (0x54, 0x21), (0x5F, 0x07)]:
        s.write(reg, value)
        if s.read(reg)[0] != value:
            raise RuntimeError('register readback mismatch: %02x' % reg)
    s.write(0x4E, 0x0F)
    time.sleep(.25)
    s.write(0x4B, 2)
    time.sleep(.002)
    s.read(0x2D)
    lost_start = s.read(0x6C, 2).hex()
    s.write(0x16, 0x40)  # stream-to-FIFO
    start = time.monotonic()
    window_start = start
    total, window = Stats(), Stats()
    delta_hist, headers = collections.Counter(), collections.Counter()
    bad = invalid = full = resets = id_bad = duplicates = 0
    max_fifo = 0
    max_poll_gap = 0.
    previous_poll = start
    prev_ts = prev_data = None
    windows = []
    result = dict(name=name, seconds_requested=seconds, spi_hz=speed,
                  odr_code=odr, filter_register='0x44', lost_start_hex=lost_start)
    with (out / (name + '.bin')).open('wb') as raw, (out / (name + '_batches.csv')).open('w') as batches:
        batches.write('host_monotonic_ns,byte_count\n')
        while time.monotonic() - start < seconds:
            now = time.monotonic()
            max_poll_gap = max(max_poll_gap, now - previous_poll)
            previous_poll = now
            # Read the complete counter in one CS assertion, as a coherent burst.
            count = int.from_bytes(s.read(0x2E, 2), 'big')
            max_fifo = max(max_fifo, count)
            if count > 2048:
                raise RuntimeError('impossible FIFO count %d' % count)
            count = count // 16 * 16
            if count:
                data = s.read(0x30, count)
                raw.write(data)
                batches.write('%d,%d\n' % (time.monotonic_ns(), count))
                for off in range(0, count, 16):
                    h, ax, ay, az, gx, gy, gz, temp, ts = struct.unpack_from('>B6hbH', data, off)
                    headers[h] += 1
                    if h & 0xFC != 0x68:
                        bad += 1
                        continue
                    vals = (ax, ay, az, gx, gy, gz)
                    if -32768 in vals or temp == -128:
                        invalid += 1
                        continue
                    if prev_ts is not None:
                        delta_hist[(ts - prev_ts) & 0xFFFF] += 1
                    prev_ts = ts
                    if vals == prev_data:
                        duplicates += 1
                    prev_data = vals
                    scaled = [x / 16384. for x in vals[:3]] + [x / 131. for x in vals[3:]] + [temp / 2.07 + 25]
                    total.add(scaled)
                    window.add(scaled)
            if now - window_start >= 10:
                status = s.read(0x2D)[0]
                full += bool(status & 2)
                resets += bool(status & 16)
                id_bad += s.read(0x75)[0] != 0x47
                windows.append(dict(end_s=now-start, duration_s=now-window_start, **window.result()))
                window, window_start = Stats(), now
                result.update(elapsed_s=now-start, stats=total.result(), windows=windows,
                              bad_headers=bad, invalid_packets=invalid, fifo_full_observations=full,
                              reset_observations=resets, id_errors=id_bad, max_fifo_bytes=max_fifo,
                              max_poll_gap_s=max_poll_gap, duplicate_axes_packets=duplicates,
                              timestamp_delta_hist=dict(delta_hist), header_hist=dict(headers))
                save(out / (name + '.json'), result)
                raw.flush()
                batches.flush()
            time.sleep(.002 if odr == 6 else .001)
        elapsed = time.monotonic() - start
        s.write(0x16, 0)
        status = s.read(0x2D)[0]
        windows.append(dict(end_s=elapsed, duration_s=start+elapsed-window_start, **window.result()))
        result.update(elapsed_s=elapsed, stats=total.result(), windows=windows,
                      rate_hz=total.n/elapsed, bad_headers=bad, invalid_packets=invalid,
                      fifo_full_observations=full+bool(status & 2), reset_observations=resets+bool(status & 16),
                      id_errors=id_bad, max_fifo_bytes=max_fifo, max_poll_gap_s=max_poll_gap,
                      duplicate_axes_packets=duplicates, timestamp_delta_hist=dict(delta_hist),
                      header_hist=dict(headers), lost_end_hex=s.read(0x6C, 2).hex(), complete=True)
        save(out / (name + '.json'), result)
    return result


def main():
    from maix.peripheral import pinmap
    p = argparse.ArgumentParser()
    p.add_argument('--out', required=True)
    p.add_argument('--seconds', type=float, default=600)
    p.add_argument('--stress-seconds', type=float, default=120)
    args = p.parse_args()
    if not 0 < args.seconds <= 3600 or not 0 <= args.stress_seconds <= 600:
        p.error('duration outside bounded test range')
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=False)
    pins = {'A0': 'SPI1_MOSI', 'A1': 'SPI1_MISO', 'A2': 'SPI1_CS0', 'A4': 'SPI1_SCK'}
    old_pins = {p: pinmap.get_pin_function(p) for p in pins}
    summary = dict(start_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                   pid=os.getpid(), original_pins=old_pins, phases=[])
    saved = {}
    s = None
    def stop(signum, frame):
        raise RuntimeError('terminated by signal %d' % signum)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        for pin, func in pins.items():
            pinmap.set_pin_function(pin, func)
        s = SPI()
        if s.read(0x75)[0] != 0x47:
            raise RuntimeError('WHO_AM_I is not 0x47')
        saved = {r: s.read(r)[0] for r in [0x16, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x52, 0x54, 0x5F]}
        summary['original_registers'] = {hex(r): v for r, v in saved.items()}
        if saved[0x4E] & 15 or saved[0x16] & 0xC0:
            raise RuntimeError('IMU already active; refusing to replace active acquisition')
        summary['self_test_config'] = s.read(0x70)[0]
        s.write(0x76, 4)
        try:
            summary['user_offsets_hex'] = s.read(0x77, 9).hex()
        finally:
            s.write(0x76, 0)
        summary['id_reads'] = 10000
        summary['id_errors'] = sum(s.read(0x75)[0] != 0x47 for _ in range(10000))
        if summary['id_errors']:
            raise RuntimeError('ID stress failed')
        s.write(0x4C, (saved[0x4C] & 0x0C) | 0x33)
        s.write(0x4D, (saved[0x4D] & 0xF8) | 1)  # internal PLL; no external CLKIN dependency
        save(out / 'summary.json', summary)
        for name, duration, odr, speed in [('static_1k', args.seconds, 6, 1000000),
                                           ('stress_4k', args.stress_seconds, 4, 4000000)]:
            if duration:
                summary['phases'].append(phase(s, out, name, duration, odr, speed))
                save(out / 'summary.json', summary)
        if any(p['stats']['n'] == 0 for p in summary['phases']):
            raise RuntimeError('phase produced no valid samples')
        summary['complete'] = True
    except Exception:
        summary['error'] = traceback.format_exc()
    finally:
        errors = []
        if s:
            try:
                if saved and not (saved[0x4E] & 15 or saved[0x16] & 0xC0):
                    s.speed = 1000000
                    s.write(0x16, 0)
                    time.sleep(.05)
                    s.write(0x4E, 0)
                    for reg, value in saved.items():
                        if reg not in (0x16, 0x4E):
                            s.write(reg, value)
                    s.write(0x16, saved[0x16])
                    s.write(0x4E, saved[0x4E])
                    summary['restored_registers'] = {hex(r): s.read(r)[0] for r in saved}
                s.close()
            except Exception as e:
                errors.append(str(e))
        for pin, func in old_pins.items():
            try:
                pinmap.set_pin_function(pin, func)
            except Exception as e:
                errors.append(str(e))
        summary['restored_pins'] = {p: pinmap.get_pin_function(p) for p in pins}
        summary['cleanup_errors'] = errors
        save(out / 'summary.json', summary)
        print(json.dumps(summary), flush=True)
    return 0 if summary.get('complete') and not errors else 1


if __name__ == '__main__':
    raise SystemExit(main())
