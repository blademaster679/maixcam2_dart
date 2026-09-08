"""Full native-array controls for the binning feasibility study.

These modes do not enable binning, skipping, or 360fps. Geometry and sampling
remain byte-for-byte equal to the full-resolution SDK baseline. Only documented
frame timing and existing same-sensor PLL multipliers may change.
"""
import hashlib
from pathlib import Path
from reverse_engineer import arrays, final_state, BASE

SETTINGS_SHA256 = 'e469a1321125685357202e211cd52c685e92f9929a0de576079c92d01314949f'


def native_candidate(path, rate):
    path=Path(path)
    if hashlib.sha256(path.read_bytes()).hexdigest()!=SETTINGS_SHA256:
        raise ValueError('Native probe requires the reviewed SDK settings hash')
    if rate not in (30,60,90):
        raise ValueError('Only 30/60/90 full-array controls have timing evidence')
    modes=arrays(path)
    base=final_state(modes[BASE])
    reference=final_state(modes['OS04A10_2lane_2688x1520_10bit_Linear_60fps'])
    if reference.get(0x0325)!=0xd8:
        raise ValueError('Missing same-sensor PLL2 reference')
    # 1640 gives 89.96fps, below the datasheet's native 90fps ceiling.
    sclk,vts={30:(72000000,3248),60:(72000000,1640),90:(108000000,1640)}[rate]
    hts=732
    ops=[list(r) for r in modes[BASE] if r[0]!=0x0100]
    ops.append([0x0100,0,0])
    if rate==90:
        # Full 90fps RAW10 needs >3.67Gbit/s payload. Baseline 4x720Mbps
        # cannot carry it. Same-sensor four-lane HDR tables use 1104Mbps/lane
        # with PLL1=0x5c and the same 0x4837=0x0e timing; leave HDR disabled.
        fast=final_state(modes['OS04A10_4lane_2688x1520_10bit_DCG_VS_HDR_30fps'])
        for a in [0x0306,0x0307,0x0308,0x030a,0x4837]:
            if base.get(a)!=fast.get(a): raise ValueError('PLL1 reference companions differ')
        if fast.get(0x0305)!=0x5c: raise ValueError('Missing 1104Mbps PLL1 reference')
        ops.extend([[0x0305,0x5c,10000],[0x0325,0xd8,10000]])
    for address,value in [(0x380e,vts),(0x3501,32)]:
        ops.extend([[address,value>>8,0],[address+1,value&255,0]])
    state=final_state(ops)
    for a in range(0x3800,0x3823):
        if a not in (0x380e,0x380f) and state.get(a)!=base.get(a):
            raise ValueError('Full-array geometry/sampling changed')
    if {a for a in state if state.get(a)!=base.get(a)} - {0x0100,0x0305,0x0325,0x380e,0x380f,0x3501,0x3502}:
        raise ValueError('Unexpected native probe modification')
    state.update({0x0328:5,0x032a:2,0x032f:0})
    p=dict(status='fullfov_timing_candidate',sensor='OS04A10',width=2688,height=1520,
           fps=sclk/hts/vts,raw_bits=10,xclk_hz=24000000,lanes=4,
           readout='native-full-array-no-binning',sclk_hz=sclk,hts=hts,vts=vts,
           min_vts=1640,exposure_margin_lines=8,lane_mbps=1104 if rate==90 else 720,bayer='RGGB',
           registers=ops,source=f'{path.resolve()}::{BASE}',source_sha256=SETTINGS_SHA256,
           keep_receiver=True,native_rate_label=rate,
           hypotheses=['Native full-array timing control, not a 360fps implementation',
                       '90fps transfers same-sensor four-lane 1104Mbps PLL1; baseline receiver config retained, verify link',
                       'All baseline window, output offsets and sampling fields retained'])
    return p,state
