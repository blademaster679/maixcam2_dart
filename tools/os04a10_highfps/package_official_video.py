#!/usr/bin/env python3
"""Package captured elementary streams at acquisition rate and at 30fps slow playback.
Frame counts are cross-checked against the recorder's acquisition and submission logs.
"""
import argparse,csv,hashlib,json,subprocess,tempfile
from fractions import Fraction
from pathlib import Path

def run(cmd): subprocess.run(cmd,check=True)
def probe(p):
 return json.loads(subprocess.check_output(['ffprobe','-v','error','-count_frames','-select_streams','v:0','-show_entries','stream=codec_name,width,height,avg_frame_rate,r_frame_rate,nb_read_frames,duration','-of','json',str(p)]))['streams'][0]
def frame_hashes(p,raw=False):
 options=['-f','rawvideo','-pixel_format','nv21','-video_size','640x360','-framerate','360'] if raw else []
 text=subprocess.check_output(['ffmpeg','-v','error','-xerror','-nostdin',*options,'-i',str(p),'-map','0:v:0','-pix_fmt','yuv420p','-vsync','0','-f','framemd5','-'],text=True)
 return [line.rsplit(',',1)[-1].strip() for line in text.splitlines() if line and not line.startswith('#')]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('run',type=Path);ap.add_argument('--out',type=Path,default=Path('artifacts/os04a10_official_20260908'));a=ap.parse_args()
 capture=json.loads((a.run/'capture.json').read_text());meta=json.loads((a.run/'run.json').read_text());mode=meta['official_mode'];fps=capture['requested_fps']
 burst=capture.get('burst_recording',False)
 if not capture.get('recording') and not burst:raise SystemExit('Not a recording run')
 acquired=list(csv.DictReader((a.run/'frames.csv').open()))
 if capture['exit_code'] or meta['exit_code']:raise SystemExit('Failed recording; preserve diagnostics instead of publishing a passing video')
 a.out.mkdir(parents=True,exist_ok=True)
 original=a.out/(mode+'-realtime.mp4');slow=a.out/(mode+'-slow30.mp4')
 for p in [original,slow]:
  if p.exists():raise SystemExit('Refusing to overwrite '+str(p))
 src=a.run/'record.h264'
 if burst:
  raw=a.run/'record.nv21'
  if raw.stat().st_size!=len(acquired)*640*360*3//2:raise SystemExit('Raw burst size/count mismatch')
  src=a.run/'host-lossless.h264'
  if src.exists():raise SystemExit('Refusing to overwrite '+str(src))
  run(['ffmpeg','-v','error','-nostdin','-f','rawvideo','-pixel_format','nv21','-video_size','640x360','-framerate',str(fps),'-i',str(raw),'-an','-c:v','libx264','-pix_fmt','yuv420p','-qp','0','-preset','fast','-vsync','0',str(src)])
 # Input -r synthesizes constant-frame-rate playback from the captured frame order.
 # Original acquisition sequence/PTS remain in separate CSV; no claim of original exposure timestamps in MP4.
 for rate,p in [(fps,original),(30,slow)]:
  # Parse the target VUI rate on a second pass so the final packet duration also
  # agrees with the MP4 playback rate. Only timing metadata changes, no re-encode.
  with tempfile.TemporaryDirectory(dir=a.out) as temporary:
   timed=Path(temporary)/'timed.h264'
   run(['ffmpeg','-v','error','-nostdin','-i',str(src),'-c:v','copy','-bsf:v',f'h264_metadata=tick_rate={rate*2}/1',str(timed)])
   run(['ffmpeg','-v','warning','-nostdin','-r',str(rate),'-i',str(timed),'-an','-c:v','copy','-video_track_timescale',str(rate*1000),'-movflags','+faststart',str(p)])
 encoded=list(csv.DictReader((a.run/'encoded_frames.csv').open())) if not burst else acquired
 analysis=json.loads((a.run/'analysis.json').read_text())
 streams={p.name:probe(p) for p in [original,slow]}
 source_hashes=frame_hashes(src)
 if burst and source_hashes!=frame_hashes(raw,raw=True):raise SystemExit('Lossless host encode changed the raw burst pixel sequence')
 for name,s in streams.items():
  if int(s['nb_read_frames'])!=len(encoded):raise SystemExit('Encoded/decoded frame count mismatch: '+name)
  expected_rate=30 if name.endswith('-slow30.mp4') else fps
  if Fraction(s['avg_frame_rate'])!=expected_rate:raise SystemExit('MP4 average frame rate mismatch: '+name)
  if frame_hashes(a.out/name)!=source_hashes:raise SystemExit('Decoded pixel sequence differs from source: '+name)
 identities=lambda rows:[(int(r['sequence']),int(r['pts_raw'])) for r in rows]
 exact_order=identities(encoded)==identities(acquired)
 all_frames_preserved=exact_order and len(source_hashes)==len(encoded)==capture['frames'] and capture.get('encoder_queue_dropped',0)==0 and analysis['sequence_gaps']==0 and analysis['sequence_duplicates']==0 and analysis['sequence_backwards']==0
 report={'run':str(a.run.resolve()),'mode':mode,'capture':capture,'encoded_index_rows':len(encoded),'streams':streams,'all_acquired_frames_preserved_and_sequence_contiguous':all_frames_preserved,
  'encoder_sequence_and_pts_match_acquisition':None if burst else exact_order,'full_decode_passed':True,'decoded_pixels_match_source_in_order':True,
  'recording_path':'One-second uncompressed NV21 RAM burst; host lossless H264 encoding after capture. This is not continuous device hardware encoding.' if burst else 'Device VIN NV12 to asynchronous AX VENC H264',
  'raw_burst_pixels_preserved_losslessly':True if burst else None,
  'timing_note':'MP4 uses frame-index CFR playback at requested fps or 30fps; original acquisition sequence/PTS/monotonic timestamps are in source CSV. Slow clips contain the same recorded frames, without interpolation. A recording with missing frames must not be presented as continuous real-time footage.',
  'sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [original,slow]}}
 (a.out/(mode+'-video-validation.json')).write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report,indent=2))
if __name__=='__main__':main()
