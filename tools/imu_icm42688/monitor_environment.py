"""Read-only telemetry, independent of Wi-Fi transport, bounded to 40 minutes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    p=argparse.ArgumentParser()
    p.add_argument('run',type=Path)
    args=p.parse_args()
    (args.run/'kernel_start.txt').write_text(subprocess.getoutput('dmesg'))
    start=time.monotonic()
    with (args.run/'environment.jsonl').open('a') as f:
        while time.monotonic()-start<2400:
            row={'monotonic_s':time.monotonic()}
            for key,path in [('boot_id','/proc/sys/kernel/random/boot_id'),
                             ('wireless','/proc/net/wireless'),('load','/proc/loadavg')]:
                try:row[key]=Path(path).read_text().strip()
                except OSError as e:row[key]=str(e)
            for key,command in [('link',['ip','-s','link','show','wlan0']),
                                ('wpa',['wpa_cli','-i','wlan0','status'])]:
                try:
                    result=subprocess.run(command,capture_output=True,text=True,timeout=2)
                    row[key]=result.stdout+result.stderr
                except (OSError,subprocess.TimeoutExpired) as e:row[key]=str(e)
            f.write(json.dumps(row)+'\n');f.flush();os.fsync(f.fileno())
            # Persist the ring buffer while connected; a reboot otherwise loses it.
            try:
                result=subprocess.run(['dmesg'],capture_output=True,text=True,timeout=2)
                target=args.run/'kernel_latest.txt'
                with target.with_suffix('.tmp').open('w') as k:
                    k.write(result.stdout);k.flush();os.fsync(k.fileno())
                target.with_suffix('.tmp').replace(target)
            except (OSError,subprocess.TimeoutExpired):pass
            try:
                summary=json.loads((args.run/'summary.json').read_text())
                if summary.get('complete') or summary.get('error'):break
            except (OSError,ValueError):pass
            time.sleep(10)
    (args.run/'kernel_end.txt').write_text(subprocess.getoutput('dmesg'))


if __name__=='__main__':
    main()
