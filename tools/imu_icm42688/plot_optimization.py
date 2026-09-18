"""Plot independently validated one-second body-axis integrals, never sample RMS."""
import os
os.environ.setdefault('MPLCONFIGDIR','/tmp/imu-matplotlib')
import argparse,csv,json,hashlib
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
p=argparse.ArgumentParser();p.add_argument('run',type=Path);a=p.parse_args()
src=a.run/'optimization.json';d=json.loads(src.read_text());out=a.run/'figures';out.mkdir(exist_ok=True)
m=d['methods'];t=np.arange(1,601)
colors={'mean_bias':'#c77700','robust_bias_lowpass':'#2166ac','rest_adaptive_lowpass':'#16846b'}
labels={'mean_bias':'Fixed mean bias','robust_bias_lowpass':'Robust bias + low-pass','rest_adaptive_lowpass':'Static adaptive + low-pass'}
plt.rcParams.update({'font.size':11,'axes.spines.top':False,'axes.spines.right':False})
with (out/'one_second_angles.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['validation_second_end','method','x_deg','y_deg','z_deg'])
 for name,v in m.items():
  for sec,xyz in zip(t,v['one_second_axis_deg']):w.writerow([sec,name,*xyz])
with PdfPages(out/'drift_optimization.pdf') as pdf:
 fig,ax=plt.subplots(3,2,figsize=(13,10),sharex=True)
 fig.suptitle('ICM-42688-P: one-second angular drift',y=.98,fontsize=18)
 fig.text(.5,.935,'Independent validation: capture 600–1200 s | tilted, externally confirmed stationary',ha='center')
 for i,axis in enumerate('XYZ'):
  ax[i,0].plot(t,np.array(m['raw']['one_second_axis_deg'])[:,i],color='#555555',lw=.9)
  ax[i,0].set_ylabel(axis+' integral (deg / 1 s)')
  for name,color in colors.items():ax[i,1].plot(t,np.array(m[name]['one_second_axis_deg'])[:,i],color=color,label=labels[name],lw=.9)
  for aa in ax[i]:aa.axhline(0,color='#aaa',lw=.6);aa.grid(alpha=.2);aa.set_xlim(0,600)
 ax[0,0].set_title('Uncompensated (axis-specific scales)');ax[0,1].set_title('Compensated (axis-specific scales)')
 ax[0,1].legend(fontsize=8,loc='best')
 for aa in ax[-1]:aa.set_xlabel('Seconds since validation started')
 fig.subplots_adjust(top=.85,bottom=.09,left=.10,right=.97,hspace=.28,wspace=.25)
 fig.savefig(out/'one_second_drift.png',dpi=160);pdf.savefig(fig);plt.close(fig)
 fig,ax=plt.subplots(3,1,figsize=(12,10),sharex=True)
 fig.suptitle('Accumulated body-axis error after compensation',y=.98,fontsize=18)
 fig.text(.5,.935,'Cumulative sums of 600 one-second integrals; these are not Euler-angle errors',ha='center')
 for i,axis in enumerate('XYZ'):
  for name,color in colors.items():ax[i].plot(np.arange(601),np.r_[0,np.cumsum(np.array(m[name]['one_second_axis_deg'])[:,i])],color=color,label=labels[name],lw=1.2)
  ax[i].set_ylabel(axis+' integral (deg)');ax[i].axhline(0,color='#aaa',lw=.6);ax[i].grid(alpha=.2);ax[i].set_xlim(0,600)
 ax[0].legend(loc='best',fontsize=9);ax[-1].set_xlabel('Seconds since validation started')
 fig.subplots_adjust(top=.85,bottom=.09,left=.10,right=.97,hspace=.23)
 fig.savefig(out/'cumulative_drift.png',dpi=160);pdf.savefig(fig);plt.close(fig)
(out/'manifest.json').write_text(json.dumps({'source':str(src),'sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'raw_sha256':d['raw_sha256'],'period_s':[600,1200],'points_per_method':600,'units':'body-axis degrees integrated over each one-second window'},indent=2))
print(out)
