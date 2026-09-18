from pathlib import Path
import subprocess,time
p=Path('/root/wifi-diagnosis-20260915')
assert Path('/sys/module/aic8800_fdrv/parameters/ps_on').read_text().strip()=='N'
assert Path('/run/systemd/system/wifi.service.d/90-ps-diagnostic.conf').exists()
subprocess.run(['systemctl','restart','wifi-ps-diagnostic-rollback.timer'],check=True)
for a,b in [('power_trial.jsonl','module_off_power_trial.jsonl'),('kernel_latest.txt','module_off_kernel.txt')]:
 if (p/b).exists():raise RuntimeError('archive exists')
 (p/a).rename(p/b)
s=p/'wifi_ps_off.sh';src=s.read_text();assert src.count('ps_on=0')==1;s.write_text(src.replace('ps_on=0','ps_on=0 he_on=0'))
subprocess.run(['systemctl','restart','wifi.service'],timeout=50)
subprocess.run(['systemctl','start','udhcpc.service'],timeout=20)
time.sleep(15)
for n in ['ps_on','he_on']:print(n,Path('/sys/module/aic8800_fdrv/parameters',n).read_text(),flush=True)
print(subprocess.getoutput('wpa_cli -i wlan0 status'),flush=True)
