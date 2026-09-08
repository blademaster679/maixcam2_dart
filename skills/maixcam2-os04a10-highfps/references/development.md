# 模式论证与实现计划

## 第一项产物：模式证据表

对每个候选模式记录以下内容及来源；未获资料写“未知”，不填猜测数值：

- 模组厂商/版本、chip ID、板级接线、供电、reset/pwdn、XCLK。
- 有效输出尺寸、读出窗口、光学黑区/embedded lines、裁剪原点、binning/subsampling、镜像/翻转和 Bayer 相位。
- Linear/HDR、RAW 位深、PLL 派生关系、HTS/VTS 及其单位、最小行/帧长、曝光和增益范围、group hold/更新生效帧。
- MIPI lane 数/映射/极性、每 lane bit rate、接收端 timing/settle、VC/DT、实际发送尺寸。
- VIN/ISP 输入帧率与处理模式、IVPS 输出、缓冲数量/stride/内存占用、可用错误计数器。

优先获取与实际模组匹配的厂商模式表；备选是从有文档依据的合法时序推导候选模式。若只有默认模式表而没有高速时序的依据，不通过缩小 VTS 或提高 PLL 试错来宣称可行。

向厂商准备的资料请求应明确：OS04A10 + MaixCAM2/AX630C + 当前 MSP 版本、640×360@360fps Linear、可接受的视野方案、完整初始化及切换序列、时钟/曝光约束、MIPI 参数与支持的模块版本。准备请求内容不等于获准发送给厂商。

## 时序和数据量初算

在手册已确认 HTS 为 pixel timing clock 周期数时：

```text
line_time = HTS / timing_clock
frame_time = VTS × line_time
fps = timing_clock / (HTS × VTS)
exposure_max ≲ (VTS - documented_margin_lines) × line_time
```

若 HTS 按多个像素或特殊时钟计数，先换算。不要把 MIPI lane bit rate 当 timing clock。曝光还需考虑 coarse/fine integration 和回调单位。

360fps 的名义帧周期约 2.7778ms，曝光上限需要减去传感器规定余量。ROI 减少输出像素不一定减少内部扫描行长/帧长；full-FOV 的 binning/subsampling 也不能在无文档时假设存在。

640×360@360 的有效数据量（十进制、不含行对齐和额外数据）：

| 格式 | 每帧 | 每秒 |
| --- | ---: | ---: |
| packed RAW10 | 288,000 B | 103.68 MB/s，即 0.82944 Gbit/s |
| packed RAW12 | 345,600 B | 124.416 MB/s |
| RAW unpacked 16-bit | 460,800 B | 165.888 MB/s |
| NV21 | 345,600 B | 124.416 MB/s |
| RGB888 | 691,200 B | 248.832 MB/s |

MIPI 预算另加实际传输尺寸、embedded data、协议开销和时序余量，按有效 lane 分摊；DDR 预算要计入每次读写和 ISP 中间缓冲。像素总量只给吞吐下界，不能证明 sensor 最短行时间或 ISP 最大帧率。

若当前有线链路是 USB2，不能把全速未压缩回传作为采集验收前提：上述 RAW10 有效负载已经超过 USB2 的 480 Mbit/s 标称速率。先在板端内存采集、汇总指标、抽样导出。编码/联网单独测量。

## 分层实现

| 层 | 工作 | 完成证据 |
| --- | --- | --- |
| Sensor driver | 在独立 SDK/MSP 副本增加有依据的模式枚举、寄存器表、模式选择；更新 set/get fps、曝光线数限制、AE 默认值和生效延迟 | 可追溯模式表、计算结果、编译通过；上板后读回允许读取的关键状态并验证真实流 |
| Axera capture | 配置 sensor 注册对象、MIPI RX、VIN dev/pipe/channel、RAW 类型、ISP 输入、crop、pool 和 stride；最小链路先禁用 AI-ISP/HDR/显示/编码 | RAW 图像尺寸/Bayer 正确；VIN 帧序和错误计数可观测 |
| NV21 output | 使用同一 sensor 模式验证 ISP/IVPS 吞吐、输出节流配置、对齐与缓冲 | 独立测得 NV21 帧率；RAW 通过而 NV21 失败时注明瓶颈层 |
| MaixCDK API | 扩展 `__get_vi_case` 与相关 sample case；请求值、实际模式、返回信息一致；曝光与 fps 含义分离 | 不支持组合返回明确错误；不会静默降为 30fps 后报告 360fps |
| Benchmark | 获取有来源说明的序号、硬件 PTS、主机 monotonic 时间；单独计数超时、释放失败、序号跳变和图像异常 | 能对低速基线、丢帧及时间戳异常作出不同结论 |

优先复用 Axera 本平台 OS04A10 驱动结构。其他传感器的高帧率模式可用来理解模式注册、pool 或接口改动，不能照抄 PLL、模拟寄存器和曝光公式。

基准程序采用预分配的有界缓冲，正确归还 SDK 帧；统计批量输出，每秒一次即可。原始帧仅短时抽样，避免存储或终端输出制造丢帧。软件统计计数器可递增，但不能当作硬件帧序号。

## 构建、部署和回退

固定 CDK commit、MSP 源码包哈希、编译器、sysroot 与库 ABI；将下载缓存中的目标源码复制到独立维护目录，以补丁记录变更。修改模式枚举后执行相应组件完整重编译，确认 AArch64 ELF、依赖和 sensor 导出符号。

部署实验库前记录原库路径/哈希、使用它的进程、ISP 参数及现有基线运行方式。优先独立应用目录和进程级加载路径，保持所需系统搜索路径，检查 `/proc/<pid>/maps` 或加载器日志确认实际库。不能仅凭复制成功认定新驱动已生效。

开始有界采集实验时独占相机，保存原应用恢复方式；正常退出释放帧和设备，超时停止实验。恢复原库/原应用并验证基线图像和帧率。只有已授权且确有必要才持久修改系统配置或刷写。

预计实施阶段：基线与资料核验 → 模式可行性报告 → 默认模式独立样例 → 有依据的高速档 → NV21 → API 集成 → 稳定性和回退验收。资料获取与传感器极限尚不确定，不预先承诺完成日期。
