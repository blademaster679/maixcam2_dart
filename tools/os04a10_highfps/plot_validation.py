#!/usr/bin/env python3
"""Plot saved benchmark evidence; never infer a successful run from requested FPS."""
import argparse
import csv
import json
import os
from pathlib import Path
os.environ.setdefault('MPLCONFIGDIR', str(Path('.maixpy/matplotlib').resolve()))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('run',type=Path)
    args=ap.parse_args()
    analysis=json.loads((args.run/'analysis.json').read_text())
    capture=json.loads((args.run/'capture.json').read_text())
    with (args.run/'health.csv').open() as f: health=list(csv.DictReader(f))
    times=[float(r['elapsed_us'])/60e6 for r in health]
    temp=[float(r['temperature_c']) if float(r['temperature_c'])!=-999 else float('nan') for r in health]
    rss=[int(r['rss_kb'])/1024 for r in health]
    windows=analysis['window_fps_10s']
    fig,axes=plt.subplots(3,1,figsize=(9,8),sharex=True,layout='constrained')
    width,height=analysis['dimensions'][0]
    mode='full FOV binning' if width==1344 else 'center crop' if width==640 else 'sensor output'
    fig.suptitle(f"OS04A10 {width} x {height} {capture['measurement']} — {mode}\n"
                 f"{analysis['monotonic_fps']:.3f} fps; gaps={analysis['sequence_gaps']}; capture status={capture['exit_code']}")
    axes[0].plot([(i+.5)/6 for i in range(len(windows))],windows,color='#146b8c',linewidth=1)
    threshold=359 if capture['requested_fps']==360 else capture['requested_fps']*.995
    axes[0].axhline(threshold,color='#777777',linestyle='--',linewidth=.8,label=f'{threshold:g} fps reference')
    axes[0].set_ylabel('Frames/s\n(complete 10s windows)')
    axes[0].legend(loc='lower right')
    axes[1].plot(times,temp,color='#c46b26',linewidth=1)
    axes[1].set_ylabel('Sensor temperature (C)')
    axes[2].plot(times,rss,color='#586c42',linewidth=1)
    axes[2].set_ylabel('Process RSS (MiB)')
    axes[2].set_xlabel('Elapsed time (min) — RSS includes retained frame metadata')
    for ax in axes:
        ax.grid(alpha=.2)
        ax.spines[['top','right']].set_visible(False)
    fig.savefig(args.run/'validation.png',dpi=160)
    fig.savefig(args.run/'validation.pdf')
    plt.close(fig)
    print(args.run/'validation.png')


if __name__=='__main__': main()
