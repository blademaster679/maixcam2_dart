#!/usr/bin/env python3
"""Analyze VIN metadata without assuming undocumented PTS units or semantics."""
import argparse
import csv
import json
import statistics
from pathlib import Path


def readback(path):
    with Path(path).open() as f:
        rows=list(csv.DictReader(f))
    if any(int(r['result']) for r in rows):
        return {'error':'register read failed'}
    r={int(row['address']):int(row['value']) for row in rows}
    word=lambda a:r[a]*256+r[a+1]
    pre=[1,1.5,2,2.5,3,4,6,8][r[0x0323]&7]
    div=[1,1.5,2,2.5,3,3.5,4,5][r[0x032a]&7]
    sclk=24e6/((r[0x0322]&1)+1)/pre*((r[0x0324]&3)*256+r[0x0325])/((r[0x032f]&15)+1)/((r[0x0328]&15)+1)/div
    raw=word(0x4f06)
    signed=raw if raw<=0xc000 else raw-0xc000
    temperature=(signed>>8)+(signed&255)/255
    if raw>0xc000: temperature=-temperature
    return {'sclk_hz_inferred_at_24mhz_xclk':sclk,'hts':word(0x380c),'vts':word(0x380e),
            'timing_fps_inferred':sclk/word(0x380c)/word(0x380e),
            'exposure_lines':word(0x3501),'width':word(0x3808),'height':word(0x380a),
            'window':[word(a) for a in [0x3800,0x3802,0x3804,0x3806]],
            'sensor_temperature_c':temperature,'temperature_note':'Datasheet section 3.8; internal sensor reading, not an external calibrated measurement'}


def analyze(rows):
    if len(rows) < 2:
        raise ValueError('At least two frames required')
    seq = [b['sequence']-a['sequence'] for a, b in zip(rows, rows[1:])]
    pts = [b['pts_raw']-a['pts_raw'] for a, b in zip(rows, rows[1:])]
    dt = [b['monotonic_us']-a['monotonic_us'] for a, b in zip(rows, rows[1:])]
    if min(dt) <= 0:
        raise ValueError('Non-monotonic acquisition timestamps')
    ordered = sorted(dt)
    # Complete 10s windows aligned to the first observed frame. Exclude the
    # trailing partial window instead of presenting its count as a full rate.
    origin=rows[0]['monotonic_us']
    window_count=(rows[-1]['monotonic_us']-origin)//10000000
    counts=[0]*window_count
    for row in rows:
        index=(row['monotonic_us']-origin)//10000000
        if index<window_count: counts[index]+=1
    def quantile(q):
        x = q*(len(ordered)-1)
        lo = int(x)
        return ordered[lo] + (ordered[min(lo+1, len(ordered)-1)]-ordered[lo])*(x-lo)
    return {
        'frames':len(rows), 'monotonic_fps':(len(rows)-1)*1e6/sum(dt),
        'dimensions': sorted({(r['width'],r['height']) for r in rows}),
        'sequence_gaps':sum(max(0,x-1) for x in seq),
        'sequence_duplicates':seq.count(0), 'sequence_backwards':sum(x<0 for x in seq),
        'pts_nonincreasing':sum(x<=0 for x in pts),
        'pts_delta_raw_median':statistics.median(pts),
        'pts_ticks_per_monotonic_second':sum(pts)*1e6/sum(dt),
        'pts_note':'SDK Payload TimeStamp; unit and capture phase undocumented in local header. No hardware FPS inferred.',
        'interval_us':{'p50':quantile(.5),'p95':quantile(.95),'p99':quantile(.99),'max':max(dt)},
        'sequence_note':'SDK input frame sequence number; reset/wrap not normalized.',
        'complete_10s_windows':len(counts),
        'window_fps_10s':[n/10 for n in counts],
        'min_window_fps_10s':min(counts)/10 if counts else None,
        'windows_below_359fps':[i for i,n in enumerate(counts) if n<3590],
        'window_note':'Aligned to first received frame; trailing incomplete window excluded.',
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    with (args.run/'frames.csv').open() as f:
        result = analyze([{k:int(v) for k,v in r.items()} for r in csv.DictReader(f)])
    for suffix in ['before','after']:
        path=args.run/f'registers_{suffix}.csv'
        if path.exists(): result['readback_'+suffix]=readback(path)
    health_path=args.run/'health.csv'
    if health_path.exists():
        with health_path.open() as f: health=list(csv.DictReader(f))
        temps=[float(r['temperature_c']) for r in health if float(r['temperature_c'])!=-999]
        rss=[int(r['rss_kb']) for r in health if int(r['rss_kb'])>=0]
        mipi=[int(r['mipi_errors']) for r in health if int(r['mipi_errors'])>=0]
        result['health']={'samples':len(health),'temperature_min_c':min(temps) if temps else None,
            'temperature_max_c':max(temps) if temps else None,'mipi_error_max':max(mipi) if mipi else None,
            'rss_min_kb':min(rss) if rss else None,'rss_max_kb':max(rss) if rss else None,
            'note':'RSS includes deliberately retained per-frame metadata; unavailable temperature sentinel is -999.'}
    output = json.dumps(result, indent=2)+'\n'
    (args.run/'analysis.json').write_text(output)
    print(output, end='')
