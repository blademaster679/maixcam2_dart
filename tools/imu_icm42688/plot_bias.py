"""Reproduce static scientific figures from the saved 10-second IMU windows."""
import csv
import hashlib
import json
import os
from pathlib import Path

os.environ.setdefault('MPLCONFIGDIR', '/tmp/imu-matplotlib')
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.backends.backend_pdf import PdfPages
import numpy as np


ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'artifacts/imu_static_retest2_20260914'
OUT = DATA / 'figures'
COLORS = ['#2368A0', '#CA6F20', '#62782E']
STYLES = ['-', '--', ':']


def main():
    font_path = '/mnt/c/Windows/Fonts/simhei.ttf'
    font_manager.fontManager.addfont(font_path)
    font_name = font_manager.FontProperties(fname=font_path).get_name()
    plt.rcParams.update({'font.family': [font_name],
                         'font.size': 11, 'axes.titlesize': 13, 'axes.labelsize': 11,
                         'axes.spines.top': False, 'axes.spines.right': False,
                         'axes.unicode_minus': False, 'pdf.fonttype': 42,
                         'savefig.facecolor': 'white', 'axes.edgecolor': '#8A929B'})
    summary = json.loads((DATA/'summary.json').read_text())
    result = json.loads((DATA/'bias_result.json').read_text())
    windows = summary['phases'][0]['windows']
    assert all(w['n'] > 0 for w in windows)
    assert sum(w['n'] for w in windows) == result['total_samples']
    t = np.array([(w['end_s']-w['duration_s']/2)/60 for w in windows])
    means = np.array([w['mean'] for w in windows])
    std = np.array([w['std'] for w in windows])
    bias = np.array(result['calibration_train']['mean'][3:6])
    residual = means[:, 3:6] - bias
    assert np.all(np.diff(t) > 0)
    OUT.mkdir(exist_ok=True)

    def axes_style(ax, ylabel, stages=False):
        ax.set_xlim(0, 30)
        ax.set_xticks(np.arange(0, 31, 5))
        ax.set_ylabel(ylabel)
        ax.grid(axis='y', color='#E1E5E9', lw=.7)
        ax.axvspan(0, 10, facecolor='#E9ECEF', alpha=.65, zorder=0)
        ax.axvspan(10, 20, facecolor='#F4F5F6', alpha=.65, zorder=0)
        for x in (10, 20):
            ax.axvline(x, color='#8A929B', lw=.8, ls='--', zorder=1)
        if stages:
            for x, text in [(5, '预热'), (15, '补偿估计'), (25, '独立验证')]:
                ax.text(x, 1.02, text, ha='center', va='bottom',
                        transform=ax.get_xaxis_transform(), color='#56616C', fontsize=10)

    def series(ax, values):
        for i, axis in enumerate('XYZ'):
            ax.plot(t, values[:, i], color=COLORS[i], ls=STYLES[i], lw=1.65, label=axis)
        ax.legend(loc='upper right', ncol=3, frameon=False)

    def frame(title, panels):
        fig, axs = plt.subplots(panels, 1, figsize=(12, 3*panels+1), sharex=True)
        fig.subplots_adjust(left=.12, right=.96, top=.84, bottom=.095, hspace=.43)
        fig.suptitle(title, x=.12, y=.975, ha='left', fontsize=19, fontweight='bold')
        fig.text(.12, .925, '2026-09-14  |  固定静止复测  |  约10秒窗口，共180点  |  1,815,249组原始样本',
                 fontsize=10.5, color='#59616A')
        axs[-1].set_xlabel('采样开始后的时间 / min')
        return fig, axs

    figs = []
    fig, axs = frame('ICM-42688-P 零漂复测总览', 4)
    axes_style(axs[0], '窗口平均角速度 / °/s', True)
    series(axs[0], means[:, 3:6])
    axs[0].set_ylim(-.95, 1.8)
    axs[0].set_title('原始零偏：三轴均值明显不为零', loc='left', pad=28)
    axes_style(axs[1], '窗口平均残差 / °/s')
    series(axs[1], residual)
    axs[1].margins(y=.22)
    axs[1].axhline(0, color='#4B535B', lw=.9)
    axs[1].set_title('扣除10–20分钟固定估计值；仅20–30分钟为独立验证', loc='left')
    axes_style(axs[2], '窗口内样本标准差 / °/s')
    series(axs[2], std[:, 3:6])
    axs[2].set_ylim(0, float(std[:, 3:6].max())*1.2)
    axs[2].set_title('测量波动：常量补偿不会降低标准差', loc='left')
    axes_style(axs[3], '窗口平均温度 / °C')
    axs[3].plot(t, means[:, 6], color='#444D57', lw=1.7)
    axs[3].set_title('温度趋势：预热末尾未满足方案热稳定条件', loc='left')
    fig.text(.12, .035, '线表示窗口统计，不是逐样本波形；未平滑。补偿仅为离线计算，未写入IMU。', fontsize=10, color='#59616A')
    figs.append(('imu_zero_drift_overview', fig))

    fig, axs = frame('三轴零偏细节与时间漂移', 3)
    halfspan = max(.025, float(np.max(np.abs(residual)))*1.15)
    for i, ax in enumerate(axs):
        axes_style(ax, f'{"XYZ"[i]}轴窗口均值 / °/s', i == 0)
        ax.plot(t, means[:, i+3], color=COLORS[i], ls=STYLES[i], lw=1.7)
        ax.axhline(bias[i], color='#59616A', lw=1, ls='--')
        ax.set_ylim(bias[i]-halfspan, bias[i]+halfspan)
        lo, hi = result['gyro_10s_mean_range_post_warmup_dps'][i]
        ax.set_title(f'{"XYZ"[i]}轴  |  固定估计值 {bias[i]:+.6f} °/s  |  预热后窗口均值峰峰值 {hi-lo:.6f} °/s',
                     loc='left', pad=28 if i == 0 else 12)
        ax.ticklabel_format(axis='y', style='plain', useOffset=False)
    fig.text(.12, .035, '三幅纵轴中心不同、跨度相同，以便比较漂移幅度；虚线为固定补偿值，不是厂商限值。', fontsize=10, color='#59616A')
    figs.append(('imu_gyro_bias_detail', fig))

    fig, axs = frame('加速度计静止观测', 3)
    axes_style(axs[0], '窗口平均加速度 / g', True)
    series(axs[0], means[:, :3])
    axs[0].set_ylim(-.05, 1.15)
    axs[0].set_title('三轴均值包含重力投影，不能直接当作三轴零偏', loc='left', pad=28)
    axes_style(axs[1], '窗口均值向量模长 / g')
    axs[1].plot(t, np.linalg.norm(means[:, :3], axis=1), color='#444D57', lw=1.7)
    axs[1].axhline(1, color='#8A929B', ls='--', lw=1, label='1g参考')
    axs[1].legend(frameon=False, loc='upper right')
    axs[1].set_title('先求窗口向量均值，再计算模长；不是逐样本模长的平均值', loc='left')
    axes_style(axs[2], '窗口内样本标准差 / mg')
    series(axs[2], std[:, :3]*1000)
    axs[2].set_ylim(0, float(std[:, :3].max())*1200)
    axs[2].set_title('三轴测量波动', loc='left')
    fig.text(.12, .035, '单姿态不能分离重力、偏置及比例因子；完整加速度校准需要六面测量。', fontsize=10, color='#59616A')
    figs.append(('imu_accelerometer', fig))

    with PdfPages(OUT/'imu_zero_drift_curves.pdf') as pdf:
        for name, fig in figs:
            fig.savefig(OUT/(name+'.png'), dpi=170)
            pdf.savefig(fig)
            plt.close(fig)
    with (OUT/'window_curves.csv').open('w', newline='') as f:
        writer = csv.writer(f)
        axes = ['ax_g', 'ay_g', 'az_g', 'gx_dps', 'gy_dps', 'gz_dps', 'temperature_C']
        writer.writerow(['window_start_s','window_end_s','midpoint_min','samples'] +
                        ['mean_'+a for a in axes] + ['std_'+a for a in axes] +
                        ['residual_gx_dps','residual_gy_dps','residual_gz_dps','norm_of_mean_accel_g'])
        for j, w in enumerate(windows):
            writer.writerow([w['end_s']-w['duration_s'], w['end_s'], t[j], w['n']] +
                            means[j].tolist()+std[j].tolist()+residual[j].tolist()+
                            [float(np.linalg.norm(means[j, :3]))])
    manifest = {'source_files_sha256': {name: hashlib.sha256((DATA/name).read_bytes()).hexdigest()
                                      for name in ['summary.json','bias_result.json']},
                'window_count': len(windows), 'samples': result['total_samples'],
                'time_coordinate': 'window midpoint, minutes since capture began',
                'smoothing': None, 'bias_dps': bias.tolist(),
                'independent_validation_interval_s': [1200, 1800],
                'artifacts': [p.name for p in sorted(OUT.iterdir()) if p.suffix in ['.png','.pdf','.csv']]}
    (OUT/'manifest.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
