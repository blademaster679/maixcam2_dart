# ICM-42688-P 板端测试

适配本项目自研底板的 SPI1：A0=MOSI、A1=MISO、A2=CS0、A4=SCK，
`/dev/spidev1.0`。依赖板端 Python 3 与 MaixPy pinmap，不依赖 Python spidev 包。

将 `spi.py`、`stress.py` 放入板端同一目录，在固定静止状态执行：

```bash
python3 stress.py --out /root/imu-tests/unique-run --seconds 600 --stress-seconds 120
python3 analyze.py /root/imu-tests/unique-run/summary.json
```

输出目录必须不存在。程序临时切换引脚，确认 ID 与休眠状态后采样，正常退出或
SIGINT/SIGTERM 时尝试恢复引脚和被修改的配置，并记录恢复读回值。不要同时运行多个
采集进程；程序不负责跨进程互斥。断电、SIGKILL 无法执行恢复。

- 开始读取 WHO_AM_I 10,000 次，保存原配置、用户 offset、自检开关。
- 内部 PLL，四线 SPI mode 0；16 字节 FIFO，±2g / ±250°/s。
- 基线：1kHz ODR、1MHz SPI；压力档：4kHz ODR、4MHz SPI。
- UI 滤波寄存器 0x52=0x44，带宽分别为 ODR/10；不等同于已验证的 100Hz ENBW。
- `*.bin` 保存 FIFO 原始流（每包 `>B6hbH`），`*_batches.csv` 保存批读取完成单调时间与字节数。
- `*.json` 每约 10 秒更新窗口统计，`summary.json` 在阶段结束时汇总；`complete` 表示
  程序完成，是否通过须另核对错误计数、原始时间戳与静止条件。
- 本轮采用单线程缓冲文件写入。实测服务间隔与 FIFO 占用用于评估该路径；
  尚未实现方案中的独立落盘线程、长期磁盘故障注入、完整自检与六面校准。

换算：加速度 raw/16384 g，角速度 raw/131 °/s，FIFO 温度 raw/2.07+25°C。
时间戳保留原始值；采样率以主机单调时钟统计。无逐样本硬件序号，不能把记录行号当作无丢样证明。

初次短测曾把 FIFO 计数低/高字节拆成两个 SPI 事务，产生空读；已改为 0x2E 起连续读
两字节，修正前数据仅用于问题定位。详情见测试报告。

单姿态加速度包含重力；不能据此计算三轴零偏。陀螺仪均值只有在静止、温度适当稳定时
才能解释为零偏。规格书 ±0.5°/s、±20mg 为典型参考，不是保证最大值。

30 分钟固定静止复测可用 `--seconds 1800 --stress-seconds 0`。
`bias_analysis.py summary.json --stationary-confirmed --validate-raw` 排除前 600 秒预热，
使用 600–1200 秒估计补偿，1200 秒后独立验证，同时给出均值漂移和热稳定条件检查。
只有现场确实确认静止才传 `--stationary-confirmed`；常量补偿不会消除样本波动。

`python3 tools/imu_icm42688/plot_bias.py` 从本地复测摘要生成三张 PNG、三页 PDF 和
窗口数据 CSV，输出到该批次 `figures/`。需要 NumPy、Matplotlib；当前使用 WSL 可访问的
`/mnt/c/Windows/Fonts/simhei.ttf` 中文字体。曲线以窗口中点为时间坐标，不是逐样本波形。

20 分钟零漂优化验证（需要 NumPy）：

```bash
python3 stress.py --out /root/imu-tests/unique-drift-run --seconds 1200 --stress-seconds 0
python3 evaluate_drift.py /root/imu-tests/unique-drift-run --confirmed-static
python3 -m unittest discover -s tools/imu_icm42688 -p 'test_drift_filter.py' -v
```

验证器检查完整原始包与时间戳连续性，以 0–300 秒预热、300–600 秒标定、
600–1200 秒独立验证，保存 optimization.json。比较原始值、均值补偿、一秒分块
均值的中位数补偿、10Hz 因果低通及静止自适应补偿。参数固定于 drift_filter.Settings。
每秒误差是完整一秒窗口的角速度积分，输出均值、RMS、绝对值 P95 和最大值，
区别于单样本角速度噪声。时间步长由总采集时长/样本数估计，没有外部校准时钟。
结果是传感器本体三轴积分，不是欧拉角或外部姿态真值误差。

倾斜静止可用于陀螺仪标定，加速度包含重力，不能强行三轴归零。
RestBias.correct() 先修正当前窗口，finish_window() 再更新下一窗口偏置。
自适应必须有外部静止确认，并满足内部稳定门限；六轴 IMU 无法可靠区分慢速绕
重力方向旋转与偏置。运动时撤销静止标志。低通减小高频噪声，不消除常量偏置，
且引入响应延迟。算法目前用于独立回放评估，不自动修改主应用。

monitor_environment.py RUN 可在采集目录创建后独立启动，每 10 秒持久化无线
状态和最近内核环形日志；采集结束退出，最长 40 分钟。断网不影响本地采集，
断电仍可能丢失尚未落盘的原始包。
