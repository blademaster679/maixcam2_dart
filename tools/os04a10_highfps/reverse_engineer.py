#!/usr/bin/env python3
"""Inventory vendor arrays and derive bounded, explicitly unverified crop probes."""
import argparse
import hashlib
import json
import re
from pathlib import Path

BASE = 'OS04A10_4lane_2688x1520_10bit_Linear_30fps'


def arrays(path):
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', Path(path).read_text(), flags=re.S)
    result = {}
    for name, body in re.findall(r'camera_i2c_reg_array\s+(\w+)\[\]\s*=\s*\{(.*?)\n\};', text, re.S):
        result[name] = [[int(a,16),int(v,16),10000 if int(a,16)==0x0103 else 0]
                       for a,v in re.findall(r'\{\s*(0x[\da-fA-F]+)\s*,\s*(0x[\da-fA-F]+)', body)]
    if BASE not in result:
        raise ValueError('Expected Axera linear baseline not found')
    return result


def final_state(ops):
    state = {}
    for a,v,_ in ops:
        if a==0x0103 and v==1: state.clear()
        state[a] = v
    return state


def candidate(path, hts, vts, full_width=False, sclk_mhz=72):
    # Documented window/timing fields plus one optional existing PLL2 setting.
    # No arbitrary MIPI or analog register sweep.
    if not 500 <= hts <= 750 or not 400 <= vts <= 3278:
        raise ValueError('Probe bounds: HTS 500..750, VTS 400..3278')
    if sclk_mhz not in (72,108):
        raise ValueError('Only existing SDK 72/108 MHz PLL2 settings are candidates')
    ops = arrays(path)[BASE]
    ops = [r for r in ops if r[0]!=0x0100]
    state = final_state(ops)
    baseline_state = dict(state)
    # Omitted high offset bytes are zero in baseline board readbacks.
    baseline_state.setdefault(0x3810,0)
    baseline_state.setdefault(0x3812,0)
    for addr,value in {0x0322:1,0x0323:2,0x0324:0,0x0325:0x90}.items():
        if state.get(addr)!=value: raise ValueError('Baseline PLL changed')
    # SDK get_sclk_from_setting fallback values: 0328=5, 032a=2, 032f=0.
    # Keep these defaults for AE calculations; do not add PLL writes to sequence.
    defaults = {0x0328:5,0x032A:2,0x032F:0}
    for a,v in defaults.items():
        if state.get(a,v)!=v: raise ValueError('Unexpected PLL divider')
    # Centered 656x376 array window, even origins and 8px output margins.
    changes = {0x3800:1024,0x3802:580,0x3804:1679,0x3806:955,
               0x3808:640,0x380A:360,0x3810:8,0x3812:8,
               0x380C:hts,0x380E:vts,0x384C:hts,0x3501:32}
    if full_width:
        changes.update({0x3800:0,0x3804:2703,0x3810:1032})
    ops.append([0x0100,0,0])
    if sclk_mhz==108:
        # Same sensor's 2-lane Linear60 table uses this PLL2 multiplier.
        # This does not establish that all of that mode's companion settings
        # are unnecessary: transfer to 4-lane crop remains an experiment.
        reference=final_state(arrays(path)['OS04A10_2lane_2688x1520_10bit_Linear_60fps'])
        if reference.get(0x0325)!=0xd8:
            raise ValueError('Expected SDK 108 MHz reference changed')
        ops.append([0x0325,0xd8,10000])
    for a,v in changes.items():
        ops.extend([[a,v>>8,0],[a+1,v&255,0]])
    state = final_state(ops)
    state.update(defaults)
    def first_pixel(regs):
        word=lambda a:regs[a]*256+regs[a+1]
        fmt=regs[0x3820]
        return ((word(0x3804)-word(0x3810)) if fmt&2 else (word(0x3800)+word(0x3810)),
                (word(0x3806)-word(0x3812)) if fmt&4 else (word(0x3802)+word(0x3812)))
    old_x,old_y=first_pixel(baseline_state)
    new_x,new_y=first_pixel(state)
    bayer=[['RGGB','GRBG'],['GBRG','BGGR']][(old_y^new_y)&1][(old_x^new_x)&1]
    fps = sclk_mhz*1000000/hts/vts
    if fps>360:
        raise ValueError('Probe exceeds requested 360fps ceiling')
    p = dict(status='reverse_engineered_candidate', sensor='OS04A10',width=640,height=360,
             fps=fps,raw_bits=10,xclk_hz=24000000,lanes=4,readout='crop-unverified',
             sclk_hz=sclk_mhz*1000000,hts=hts,vts=vts,min_vts=400,exposure_margin_lines=8,
             lane_mbps=720,bayer=bayer,registers=ops,
             source=f'{Path(path).resolve()}::{BASE}',source_sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest(),
             hypotheses=['ROI margins and minimum blanking require hardware validation',
                         'No binning enabled; reduced field of view',
                         f'{sclk_mhz} MHz PLL2 derived from same-sensor SDK; verify register readback',
                         'Keep baseline receiver/MIPI configuration; 108MHz case changes only PLL2 multiplier'],
             keep_receiver=True,full_width=full_width,
             bayer_basis={'baseline_pattern':'RGGB','baseline_first_pixel':[old_x,old_y],
                          'crop_first_pixel':[new_x,new_y],
                          'note':'Phase derived from baseline ROI, mirror/flip and output offsets; color-chart verification remains required'})
    return p,state


def inventory(path):
    modes = arrays(path)
    result = {}
    for name,ops in modes.items():
        s=final_state(ops)
        def word(a): return s.get(a,0)*256+s.get(a+1,0)
        result[name] = dict(writes=len(ops),width=word(0x3808),height=word(0x380A),
                            hts=word(0x380C),vts=word(0x380E),
                            window=[word(a) for a in [0x3800,0x3802,0x3804,0x3806]])
    base=final_state(modes[BASE])
    other=final_state(modes['OS04A10_4lane_2688x1520_10bit_Linear_60fps'])
    return {'modes':result,'linear_30_to_60_diff':{hex(a):[base.get(a),other.get(a)]
            for a in sorted(base.keys()|other.keys()) if base.get(a)!=other.get(a)}}


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('settings',type=Path)
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(inventory(args.settings),indent=2)+'\n')
