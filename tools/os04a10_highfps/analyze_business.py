#!/usr/bin/env python3
"""Audit business stage rates and software latency; never infer accuracy/exposure time."""
import argparse,csv,json,math
from pathlib import Path

def quantiles(values):
    a=sorted(values)
    if not a:return None
    def q(p):
        at=(len(a)-1)*p;i=int(at);return a[i]+(a[min(i+1,len(a)-1)]-a[i])*(at-i)
    return {'p50':q(.5),'p95':q(.95),'p99':q(.99),'max':a[-1]}

def fps(times):
    return (len(times)-1)*1e6/(times[-1]-times[0]) if len(times)>1 and times[-1]>times[0] else None

def proc_table(path,section):
    if not path.exists():return None
    lines=path.read_text().splitlines()
    for i,line in enumerate(lines):
        if line.strip()==section and i+2<len(lines):
            return dict(zip(lines[i+1].split(),lines[i+2].split()))
    return None

def read_csv(path):
    with path.open() as f: return list(csv.DictReader(f))

def analyze(root):
    capture=json.loads((root/'capture.json').read_text())
    frames=read_csv(root/'frames.csv')
    vision=read_csv(root/'vision.csv')
    times=[int(r['monotonic_us']) for r in frames]
    if len(times)<2:raise ValueError('Insufficient input frames')
    begin,end=times[0],times[-1];seconds=(end-begin)/1e6
    targets=[]
    with (root/'targets.jsonl').open() as f:
        for line in f:
            r=json.loads(line)
            if begin<=r['timestamp_us']<=end:
                targets.append({k:r[k] for k in ['timestamp_us','source_received_us','source_metadata_valid','safe_for_control']})
    valid_sources=[r for r in targets if r['source_metadata_valid']]
    health=read_csv(root/'health.csv')
    summary=json.loads((root/'business.json').read_text()) if (root/'business.json').exists() else None
    lease=json.loads((root/'leases.json').read_text())
    def rate(field):return sum(int(r[field]) for r in vision)/seconds
    def stage(field):return quantiles([float(r[field])/1000 for r in vision])
    seq=[int(r['sequence']) for r in frames];pts=[int(r['pts_raw']) for r in frames]
    gaps=sum(max(0,b-a-1) for a,b in zip(seq,seq[1:]));dup=sum(a==b for a,b in zip(seq,seq[1:]));rev=sum(b<a for a,b in zip(seq,seq[1:]))
    result={
      'run':str(root),'seconds':seconds,'capture_exit_code':capture['exit_code'],
      'interrupted':capture.get('interrupted',False),
      'negative_software_source_age_count':sum(r['timestamp_us']<r['source_received_us'] for r in valid_sources),
      'output_timestamp_nonincreasing':sum(b['timestamp_us']<=a['timestamp_us'] for a,b in zip(targets,targets[1:])),
      'sensor_vin_ife_end_snapshot':proc_table(root/'vin_statistics_after.txt','[IFE]'),
      'vin_itp_end_snapshot':proc_table(root/'vin_statistics_after.txt','[ITP]'),
      'nv21_channel_end_snapshot':proc_table(root/'vin_statistics_after.txt','[CHN]'),
      'application_get_fps':fps(times),'nv21_get_frames':len(frames),
      'pts_raw_ticks_per_second':(pts[-1]-pts[0])/seconds,
      'sequence_missing':gaps,'sequence_duplicate':dup,'sequence_reversed':rev,
      'pts_nonincreasing':sum(b<=a for a,b in zip(pts,pts[1:])),
      'acquire_errors':capture['acquire_errors'],'leases':lease,'business':summary,
      'green_detection_call_hz':rate('green_ran'),'direct_green_observation_hz':rate('direct_green'),
      'armor_detection_call_hz':rate('armor_ran'),'full_search_hz':rate('search_ran'),
      'motion_call_hz':rate('motion_ran'),'control_output_fps':fps([r['timestamp_us'] for r in targets]),
      'software_source_age_at_vision_start_ms':quantiles([(int(r['started_us'])-int(r['received_us']))/1000 for r in vision]),
      'software_receive_to_vision_finish_ms':quantiles([(int(r['finished_us'])-int(r['received_us']))/1000 for r in vision]),
      'software_source_age_at_control_ms':quantiles([(r['timestamp_us']-r['source_received_us'])/1000 for r in valid_sources]),
      'source_exposure_age_ms':None,'exposure_to_control_latency_ms':None,
      'control_output_semantics':'TargetEstimate generation timestamps; JSONL disk visibility and UART/CAN transport are not measured.',
      'telemetry_io':{p.name:json.loads(p.read_text()) for p in sorted(root.glob('*.io.json'))},
      'timing_limit':'Host receive timestamps are lower bounds on source age. SDK payload PTS exposure phase and cross-clock offset are unverified.',
      'stage_ms':{key:stage(key) for key in ['search_us','roi_convert_us','detect_us','motion_us']},
      'rss_kb':quantiles([int(r['rss_kb']) for r in health]),
      'rss_first_last_kb':[int(health[0]['rss_kb']),int(health[-1]['rss_kb'])] if health else None,
      'temperature_c':quantiles([float(r['temperature_c']) for r in health if float(r['temperature_c'])>-100]),
      'mipi_errors_max':max((int(r['mipi_errors']) for r in health),default=None),
      'cpu_percent_one_core':None,'cpu_khz':None,
      'safe_for_control_true':sum(r['safe_for_control'] for r in targets),
      'captured_target_invalid_gap_ms':None,'accuracy':'unmeasured: no human ground truth',
      'launcher_after':(root/'mode-after.log').read_text().strip() if (root/'mode-after.log').exists() else None,
    }
    if len(health)>1 and 'process_cpu_s' in health[0]:
        result['cpu_percent_one_core']=100*(float(health[-1]['process_cpu_s'])-float(health[0]['process_cpu_s']))/((int(health[-1]['elapsed_us'])-int(health[0]['elapsed_us']))/1e6)
        result['cpu_khz']=quantiles([int(r['cpu_khz']) for r in health if int(r['cpu_khz'])>0])
    if (root/'motion.csv').exists():
        motion=read_csv(root/'motion.csv')
        result['motion_call_hz']=len(motion)/seconds
        result['stage_ms']['motion_worker_us']=quantiles([(int(r['finished_us'])-int(r['started_us']))/1000 for r in motion])
        result['stage_ms']['motion_us_meaning']='vision-side Y thumbnail preparation and dispatch only'
    if vision and 'green_us' in vision[0]:
        result['stage_ms']['green_us']=stage('green_us')
        result['stage_ms']['armor_and_fusion_us']=quantiles([(float(r['detect_us'])-float(r['green_us']))/1000 for r in vision])
    result['capture_continuity_pass']=capture['exit_code']==0 and not(gaps or dup or rev or result['pts_nonincreasing'] or result['acquire_errors'] or lease['map_errors'] or lease['release_errors']) and lease['acquired']==lease['released']
    result['competition_acceptance_pass']=False # calibration, truth and exposure latency not provided
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('run',type=Path);a=p.parse_args()
    result=analyze(a.run);dest=a.run/'business-analysis.json';dest.write_text(json.dumps(result,indent=2)+'\n');print(dest)
    for key in ['seconds','application_get_fps','green_detection_call_hz','armor_detection_call_hz','full_search_hz','motion_call_hz','control_output_fps','sequence_missing','software_source_age_at_control_ms','rss_first_last_kb','temperature_c','capture_continuity_pass']:print(key,result[key])
if __name__=='__main__':main()
