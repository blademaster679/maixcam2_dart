"""Bounded gateway reachability evidence, without reconnecting or changing Wi-Fi."""
import argparse,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--seconds',type=int,default=1800);a=p.parse_args()
start=time.monotonic()
with a.output.open('a') as f:
 while time.monotonic()-start<a.seconds:
  row={'monotonic_s':time.monotonic(),'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
  for key,cmd in [('gateway_ping',['ping','-n','-c','1','-W','2','192.168.137.1']),('neighbors',['ip','neigh','show','dev','wlan0'])]:
   try:
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=3);row[key]={'rc':r.returncode,'text':r.stdout+r.stderr}
   except (OSError,subprocess.TimeoutExpired) as e:row[key]={'error':str(e)}
  f.write(json.dumps(row)+'\n');f.flush();os.fsync(f.fileno());time.sleep(10)
