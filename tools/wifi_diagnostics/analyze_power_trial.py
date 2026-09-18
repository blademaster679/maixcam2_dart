import argparse,json
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument('client',type=Path);a=p.parse_args()
rows=[json.loads(l) for l in a.client.read_text().splitlines() if l.startswith('{')]
result=[]
for phase in sorted({r['phase'] for r in rows if 'phase'in r}):
 rtt=[r['ms'] for r in rows if r['kind']=='rtt' and r['phase']==phase]
 workers=[r for r in rows if r['kind']=='worker' and r['phase']==phase and not r['idle']]
 end=[r for r in rows if r['kind']=='load_end' and r['phase']==phase]
 if not end:continue
 result.append(dict(phase=phase,idle_rtt_count=len(rtt),idle_rtt_median_ms=float(np.median(rtt)) if rtt else None,idle_rtt_p95_ms=float(np.percentile(rtt,95)) if rtt else None,load_seconds=end[0]['elapsed_s'],verified_bytes_each_direction=sum(r['bytes_each_direction'] for r in workers),verified_mbps_each_direction=sum(r['bytes_each_direction'] for r in workers)*8/end[0]['elapsed_s']/1e6,load_errors=sum(r['errors'] for r in workers),workers=len(workers)))
print(json.dumps(result,indent=2))
