"""Bounded echo trial with nl80211 power control and attempted normal-exit state restoration."""
import socket,threading,time,json,struct,os,subprocess
from pathlib import Path
from nl_power import Power
root=Path(__file__).resolve().parent
lock=threading.Lock();power_lock=threading.Lock();stop=threading.Event();n=Power();original=n.get();start=time.monotonic()
def log(row):
 row['monotonic_s']=time.monotonic();row['elapsed_s']=row['monotonic_s']-start
 with lock:
  with (root/'power_trial.jsonl').open('a') as f:f.write(json.dumps(row)+'\n');f.flush();os.fsync(f.fileno())
def exact(c,n):
 b=bytearray()
 while len(b)<n:
  x=c.recv(n-len(b))
  if not x:raise EOFError()
  b.extend(x)
 return b

def worker(c):
 try:
  c.settimeout(4);mode=exact(c,1)
  if mode==b'P':
   requested=exact(c,1)[0]
   if requested not in (0,1):raise ValueError('invalid power state')
   with power_lock:actual=n.set(requested)
   log({'event':'set_power','requested':requested,'actual':actual});c.sendall(bytes([actual]))
  elif mode==b'E':
   while not stop.is_set():
    size=struct.unpack('<I',exact(c,4))[0]
    if not 1<=size<=65536:raise ValueError('invalid size')
    b=exact(c,size);c.sendall(b)
  elif mode==b'Q':stop.set()
 except EOFError:pass
 except Exception as e:log({'event':'worker_error','error':str(e)})
 finally:c.close()

def monitor():
 while not stop.is_set():
  row={'event':'telemetry','wireless':Path('/proc/net/wireless').read_text(),'snmp':Path('/proc/net/snmp').read_text(),'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
  for key,cmd in [('wpa',['wpa_cli','-i','wlan0','status']),('signal',['wpa_cli','-i','wlan0','signal_poll'])]:
   try:row[key]=subprocess.run(cmd,capture_output=True,text=True,timeout=2).stdout
   except Exception as e:row[key]=str(e)
  log(row)
  (root/'kernel_latest.txt').write_text(subprocess.getoutput('dmesg'))
  stop.wait(5)
try:
 log({'event':'start','original_power':original});threading.Thread(target=monitor,daemon=True).start()
 with socket.socket() as s:
  s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('0.0.0.0',5210));s.listen(12);s.settimeout(1)
  while time.monotonic()-start<600 and not stop.is_set():
   try:
    c,addr=s.accept()
    if addr[0]!='192.168.137.1':c.close();continue
    threading.Thread(target=worker,args=(c,),daemon=True).start()
   except socket.timeout:pass
finally:
 stop.set()
 with power_lock:restored=n.set(original)
 log({'event':'finish','restored_power':restored})
