"""Validate an externally supplied OS04A10 mode; never synthesize analog settings."""
import hashlib
import json
import math
from pathlib import Path


class ProfileError(ValueError):
    pass


def validate(p):
    if p.get('status') != 'vendor_sequence_supplied':
        raise ProfileError('Missing vendor 640x360@360 RAW10 binning initialization sequence')
    for key, value in {'sensor': 'OS04A10', 'width': 640, 'height': 360,
                       'raw_bits': 10, 'xclk_hz': 24000000, 'lanes': 4,
                       'readout': '2x2-binning-crop', 'fps': 360}.items():
        if p.get(key) != value:
            raise ProfileError(f'{key} must be {value!r}')
    if not isinstance(p.get('source'), str) or not p['source'].strip():
        raise ProfileError('A traceable mode-setting source is required')
    for key in ['hts', 'vts', 'min_vts', 'exposure_margin_lines', 'lane_mbps']:
        if type(p.get(key)) is not int or p[key] <= 0:
            raise ProfileError(f'{key} must be a positive integer')
    clock = p.get('sclk_hz')
    if type(clock) not in (int, float) or not math.isfinite(clock) or not 0 < clock <= 108000000:
        raise ProfileError('SCLK must be finite and within the documented 108 MHz ceiling')
    if not 360 + p['exposure_margin_lines'] <= p['min_vts'] <= p['vts'] <= 65535:
        raise ProfileError('Invalid frame length/minimum/exposure margin')
    if p['exposure_margin_lines'] != 8 or p['hts'] > 65535 or p['lane_mbps'] > 1104:
        raise ProfileError('Timing parameter outside documented bounds')
    if abs(clock / p['hts'] / p['vts'] - 360) > 0.5:
        raise ProfileError('HTS/VTS/SCLK do not describe 360fps')
    # A lower throughput bound, not a substitute for receiver timing validation.
    if 640 * 360 * 360 * 10 >= p['lanes'] * p['lane_mbps'] * 1000000:
        raise ProfileError('Insufficient MIPI payload capacity')
    ops = p.get('registers')
    if not isinstance(ops, list) or not ops:
        raise ProfileError('Full ordered register sequence is required')
    state = {}
    for row in ops:
        if not isinstance(row, list) or len(row) != 3 or any(type(x) is not int for x in row):
            raise ProfileError('Register rows must be [address, value, delay_us] integers')
        addr, value, delay = row
        if not 0 <= addr <= 65535 or not 0 <= value <= 255 or not 0 <= delay <= 1000000:
            raise ProfileError('Register address/value/delay out of range')
        if addr == 0x0100 and value != 0:
            raise ProfileError('Initialization must stay in standby; SDK owns stream-on')
        if addr == 0x0103 and value == 1:
            state.clear()
        state[addr] = value
    def u16(addr):
        try:
            return (state[addr] << 8) | state[addr + 1]
        except KeyError as e:
            raise ProfileError(f'Missing final register state at {addr:#06x}') from e
    for addr, key in [(0x3808, 'width'), (0x380A, 'height'), (0x380C, 'hts'), (0x380E, 'vts')]:
        if u16(addr) != p[key]:
            raise ProfileError(f'Registers disagree with {key}')
    if state.get(0x0100) != 0:
        raise ProfileError('Explicit standby state required')
    if u16(0x3501) > p['vts'] - p['exposure_margin_lines']:
        raise ProfileError('Initial exposure exceeds frame budget')
    if u16(0x384C) == 0:
        raise ProfileError('Explicit nonzero VS HTS required by the SDK AE parser')
    if any(a not in state for a in [0x0322, 0x0323, 0x0324, 0x0325, 0x0328, 0x032A, 0x032F, 0x376C]):
        raise ProfileError('Missing explicit PLL2/linear gain state')
    pre = [1, 1.5, 2, 2.5, 3, 4, 6, 8][state[0x0323] & 7]
    div = [1, 1.5, 2, 2.5, 3, 3.5, 4, 5][state[0x032A] & 7]
    derived = (p['xclk_hz'] / ((state[0x0322] & 1) + 1) / pre *
               (((state[0x0324] & 3) << 8) | state[0x0325]) /
               ((state[0x032F] & 15) + 1) / ((state[0x0328] & 15) + 1) / div)
    if abs(derived - clock) > max(1, clock * 0.00001):
        raise ProfileError('Declared SCLK disagrees with PLL2 register state')
    # Validate these against the supplied mode note as well: they cannot be
    # inferred from the published summary/register map alone.
    if p.get('bayer') not in ['RGGB', 'GRBG', 'GBRG', 'BGGR']:
        raise ProfileError('Explicit Bayer order required')
    return p, state


def load(path):
    return validate(json.loads(Path(path).read_text()))


def header(profile=None, candidate_state=None):
    prefix = '#pragma once\n'
    if profile is None:
        return prefix + '#define OS04A10_HFR_AVAILABLE 0\n#define OS04A10_HFR_FPS 360.0f\n#define OS04A10_HFR_WIDTH 640\n#define OS04A10_HFR_HEIGHT 360\n'
    p, state = validate(profile) if candidate_state is None else (profile, candidate_state)
    out = [prefix, '#define OS04A10_HFR_AVAILABLE 1']
    out.append(f'#define OS04A10_HFR_FPS {p["fps"]:.10f}f')
    out.append(f'#define OS04A10_KEEP_RECEIVER {int(p.get("keep_receiver", False))}')
    for key in ['width', 'height', 'hts', 'vts', 'min_vts', 'sclk_hz', 'exposure_margin_lines', 'lane_mbps']:
        out.append(f'#define OS04A10_HFR_{key.upper()} {p[key]}')
    out.append(f'#define OS04A10_HFR_BAYER AX_BP_{p["bayer"]}')
    out += ['#ifdef OS04A10_HFR_DRIVER_TABLES',
            'typedef struct { unsigned addr, value, delay_us; } os04a10_hfr_op;',
            'static const os04a10_hfr_op os04a10_hfr_sequence[] = {']
    out += [f'    {{0x{a:04x}, 0x{v:02x}, {d}}},' for a, v, d in p['registers']]
    out += ['};', 'static camera_i2c_reg_array os04a10_hfr_final_state[] = {']
    # Drivers must compute AE defaults from final state, not the first write.
    out += [f'    {{0x{a:04x}, 0x{v:02x}}},' for a, v in sorted(state.items())]
    out += ['};', '#endif', '']
    return '\n'.join(out)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
