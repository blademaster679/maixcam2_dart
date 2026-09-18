"""Run on device: temporary systemd override with an independent rollback timer."""
from pathlib import Path
import subprocess,os,json,time
root=Path('/root/wifi-diagnosis-20260915')
override=Path('/run/systemd/system/wifi.service.d/90-ps-diagnostic.conf')
if override.exists():raise RuntimeError('override already exists')
src=Path('/opt/scripts/wifi.sh').read_text()
old='insmod /soc/ko/aic8800_fdrv.ko'
assert src.count(old)==1
script=root/'wifi_ps_off.sh';script.write_text(src.replace(old,old+' ps_on=0'));script.chmod(0o700)
rollback=root/'rollback_ps.sh'
rollback.write_text('''#!/bin/bash
exec >> /root/wifi-diagnosis-20260915/rollback.log 2>&1
date -Iseconds
rm -f /run/systemd/system/wifi.service.d/90-ps-diagnostic.conf
systemctl daemon-reload
systemctl restart wifi.service
systemctl start udhcpc.service
sleep 15
cat /sys/module/aic8800_fdrv/parameters/ps_on
wpa_cli -i wlan0 status
''');rollback.chmod(0o700)
subprocess.run(['systemd-run','--unit=wifi-ps-diagnostic-rollback','--on-active=8min','/bin/bash',str(rollback)],check=True)
override.parent.mkdir(parents=True,exist_ok=True)
override.write_text('[Service]\nExecStart=\nExecStart=/bin/bash '+str(script)+' start\n')
subprocess.run(['systemctl','daemon-reload'],check=True)
print('rollback timer armed; temporary override installed',flush=True)
subprocess.run(['systemctl','restart','wifi.service'],timeout=50)
subprocess.run(['systemctl','start','udhcpc.service'],timeout=20)
time.sleep(15)
print('ps_on',Path('/sys/module/aic8800_fdrv/parameters/ps_on').read_text(),flush=True)
print(subprocess.getoutput('wpa_cli -i wlan0 status'),flush=True)
