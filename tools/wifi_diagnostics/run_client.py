import argparse,subprocess,pathlib,base64
p=argparse.ArgumentParser();p.add_argument('label',choices=['module_off','module_he_off','module_on_restored']);a=p.parse_args()
out=pathlib.Path('artifacts/wifi_diagnosis_20260915');s=pathlib.Path('tools/wifi_diagnostics/power_trial_client.ps1').read_text().replace('phase<4','phase<2').replace('int ps=phase%2==0?1:0;','int ps=1;')
(out/(a.label+'.ps1')).write_text(s)
with (out/(a.label+'.jsonl')).open('wb') as f,(out/(a.label+'.err')).open('wb') as e:
 r=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoLogo','-NoProfile','-NonInteractive','-EncodedCommand',base64.b64encode(s.encode('utf-16le')).decode()],stdout=f,stderr=e,timeout=190)
print(a.label,'exit',r.returncode)
