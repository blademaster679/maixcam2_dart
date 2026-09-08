# OS04A10 / MaixCAM2 高帧率驱动研发

2026-09-08业务进展：官方全视野180fps已接入独立VIN异步检测入口，完成一次30分钟业务采集连续性复测。命令见文末，完整结果与未通过项见[业务验证报告](../../reports/BUSINESS_FULL180_2026-09-08.md)。下列逆向说明保留历史口径，不再作为当前模式资料缺失的判断。

2026-09-08 最新[360fps复测与修复报告](OFFICIAL_RETEST.md)：官方640×360模式将ITP内部源队列从1增加到4、保持输出队列8后，**NV21连续30分钟647,698帧、平均359.832fps，无序号异常与MIPI错误**。360fps连续硬件编码仍因队列积压未通过。官方1344×760@180全视野NV21也已通过30分钟，前次失败对照及录像保留在[初测报告](OFFICIAL_VALIDATION.md)。来源差异见[官方提交对比](OFFICIAL_COMPARISON.md)。下文保留原有驱动的研发记录。

历史逆向分支状态：**逆向中心裁剪模式已完成640×360 RAW的30分钟采集：647,698帧、平均359.832fps，无序号异常。长测SSH退出等待超时后补取证据，修复后短测正常退出；尚不作为生产驱动发布。** 该模式通过现有源码与手册字段推导，没有使用厂商360fps binning表。


2026-09-08：[全视野模式研究与上板尝试](FULL_FOV.md)已完成原生2688×1520约89.958fps的30秒RAW对照。全视野640×360@360fps仍未实现；新增同型号源码/ELF序列分析器及30/60/90全幅时序候选。
最新配方、逆向过程、失败对照与测量结果见 [逆向开发记录](REVERSE_ENGINEERING.md)。使用 `build.py --recipe crop_360_recipe.json` 构建可复现的裁剪候选；下方 vendor profile 路径用于另一条厂商 binning 模式研发线。

具体帧率、库路径、回退状态及原始日志入口见 [板端验证记录](VALIDATION.md)。

仍缺少完整的厂商360fps binning初始化序列。`target_mode.json` 是该模式的资料缺口清单，不是可运行模式表；不指定配方或profile的默认构建仍明确拒绝 `--hfr`。

## 已找到的模式依据

Sipeed 下载站提供的 OS04A10 rev 1B preliminary specification v1.0，PDF 第 59 页 table 5-1，列出 640×360、360fps、2×2 binning + cropping、720Mbps/lane。它证明传感器存在目标模式能力，不能代替适配本模组的完整寄存器序列及 MaixCAM2 实测。

- [Sipeed OS04A10 下载目录](https://dl.sipeed.com/shareURL/MaixCAM/Sensors/OS04A10)
- [完整规格 PDF](https://dl.sipeed.com/fileList/MaixCAM/Sensors/OS04A10/OS04A10-rev-1B-Preliminary-Specification-Fan-out_Version-1-0_Waching.pdf)
- [Sipeed 相机能力说明](https://wiki.sipeed.com/hardware/en/maixcam/cameras.html)

本地资料保存在 `.maixpy/datasheets/`。已检查的本地 Axera 驱动没有目标模式表。本次中心裁剪路线另外适配了PLL、窗口、AE、VIN及内存池；单独设置MaixPy的 `fps` 参数不能启用它。

## 代码与边界

| 文件 | 用途 |
| --- | --- |
| `build.py` | 从固定 MSP 版本复制源码，在独立副本应用扩展，生成 `.so`、补丁、哈希及构建记录 |
| `profile.py` | 校验来源字段、时序算式、最终寄存器状态、曝光预算、待机顺序；生成模式头文件 |
| `hfr_sensor.inc` | 高速模式专用帧长检查，保留原有共享 ABI 的 60fps 数组边界 |
| `capture.cpp` | 显式加载传感器对象、核验 chip ID/回调路径、VIN IFE RAW 采集、序号和时间戳记录 |
| `build_capture.py` | 复用当前项目的 CDK 编译/链接参数构建独立程序，测试进程关闭 AI-ISP |
| `run_device.py` | 使用 MaixPy skill 的 SSH/SCP/launcher helper，隔离部署、核验依赖、执行与恢复 |
| `analyze.py` | 统计真实取帧间隔、帧序号跳变/重复/倒退、PTS 连续性 |
| `test_tools.py` | 主机校验测试；合成数据仅检查编译与校验逻辑，严禁上板 |
| `reverse_engineer.py` | 分析现有模式表，并生成限定参数范围的实验裁剪候选 |
| `crop_360_recipe.json` | 固定源文件哈希的约360fps中心裁剪构建配方 |
| `preview_nv21.py` | NV21诊断预览，BT.601有限范围假设 |
| `plot_validation.py` | 从原始分析和健康记录生成帧率、温度、RSS曲线 |
| `preview_raw.py` | 对实测 packed RAW10 作诊断性灰度解包预览，不代替彩色标定 |

Sensor 扩展包含独立模式枚举、模式分发、有序寄存器写入与延时、错误传播、AE 帧率限制适配和慢快门禁用。没有修改 `SNS_MAX_FRAME_RATE`：该宏参与共享结构体数组大小，直接改成 360 会破坏 ABI。

厂商profile导入校验只能发现已编码的矛盾，**不能证明未公开模拟/binning寄存器正确**。该导入路径的范围为24MHz XCLK、四lane、Linear RAW10、master、2×2 binning + crop、8行曝光余量；其他条件需进一步适配。逆向recipe另行限定为纯裁剪，允许经过源码核对的72/108MHz时钟候选，两条路径的来源和验证结论分开记录。

RAW 程序复用 SDK 的 VIN/ISP 初始化链路，在 IFE 点取 RAW；它不是完全移除 ISP 的新管线。裁剪候选的内存池按640×360配置，默认队列16帧、额外8帧供ISP处理。`--nv21` 已通过独立短测，使用4帧通道队列；RAW默认队列16不可直接套用至NV21，其具体结果另行记录；公共MaixCDK `Camera` API高速模式集成尚未完成。

## 构建与复现

在项目根目录执行。依赖当前已生成的 `build/`、本地 MaixCDK 和其 AArch64 工具链；脚本不是任意版本 SDK 的通用构建器。默认 SDK 为 `~/maix/MaixCDK`，MSP 固定 `3.0.0_20250319114413`。

```bash
python3 -B tools/os04a10_highfps/build.py --out .maixpy/os04a10-new-build
python3 -B tools/os04a10_highfps/build_capture.py --driver-build .maixpy/os04a10-new-build
python3 -B -m unittest discover -s tools/os04a10_highfps -p test_tools.py -v
```

构建目录已存在时拒绝覆盖，以保留实验记录。最新裁剪构建位于 `.maixpy/crop360-final/`；默认模式回退测试构建位于 `.maixpy/os04a10-baseline-reverse/`。历史构建的采集程序可能不支持最新参数，复测前运行 `build_capture.py`。

设备地址/账户由 `.maixpy/config.json` 与 MaixPy helper 管理，不在源码中保存密码。已按用户提供的 skill 连通有线设备。若系统没有 `sshpass`，本次下载的主机工具可这样加入进程 PATH：

```bash
export PATH="$PWD/.maixpy/host-tools/sshpass/usr/bin:$PATH"
python3 -B tools/os04a10_highfps/run_device.py --driver-build .maixpy/os04a10-build-v3 --seconds 10
python3 -B tools/os04a10_highfps/run_device.py --driver-build .maixpy/os04a10-build-v3 --seconds 30 --experimental --sample-raw
python3 -B tools/os04a10_highfps/analyze.py .maixpy/runs/<run-directory>
```

默认使用 `/opt/lib/libsns_os04a10.so`；`--experimental` 显式使用该次 `/tmp` 目录中的新库。SDK 本身使用绝对路径加载 sensor，仅改变 `LD_LIBRARY_PATH` 不足以完成切换，因此采集程序替换 sensor 注册对象并通过 `dladdr` 核实实际回调路径。

每次测试暂停启动器、结束后恢复原先运行的启动器，拒绝挤占检测到的其他应用。测试超时有终止兜底，不覆盖系统库、不刷机。若主机进程被强制杀死或 SSH 断开，自动恢复可能未执行；重新连接后运行 skill 的 `mode status`/`mode exit`。拉取证据后会清理该轮临时二进制和传输包，避免填满板端224MB的 `/tmp`；远端测试数据目录保留供诊断，重启后可能消失。

`--sample-raw` / `--sample-frame` 只在预热阶段抽取一帧，默认关闭。文件为 `sample.raw`（NV21模式为 `sample.nv21`）及描述尺寸/格式/stride的 `sample.json`。格式枚举133是SDK packed RAW10，诊断脚本按从数据推断的小端连续10bit布局解包，已能观察场景结构；Bayer及受控动态场景尚未验收。

## 测量解释

预热2秒；测量期保存有界内存中的元数据，结束后写CSV。每秒采集一次健康数据，每约10秒写少量 `progress.json` 进度供主机查看。`monotonic_fps=(N-1)/(最后与最初取帧时刻差)`。SDK字段为input frame sequence number和Payload TimeStamp；头文件未明确PTS单位或SOF/EOF位置，分析保留原值。

`capture.json` 包含取帧/归还错误，`analysis.json` 包含序号连续性和 P50/P95/P99/最大间隔。`vin_statistics_*`、`mipi_rx_status_*` 保留硬件统计。未消费的 ISP 输出通道可能报告丢弃，与 IFE RAW 丢帧分开判断。短测不能替代 30 分钟稳定性、温升、冷启动或 NV21 验收。

## 厂商 binning 模式与后续工作

若需要厂商binning模式而不采用本次较小视野的纯裁剪路线，仍需Sipeed/OMNIVISION提供实际模组的完整 **640×360@360fps Linear RAW10、24MHz XCLK、四lane** 初始化及切换序列，包含写入次序/延时、binning/ROI、HTS/VTS/最小帧长、PLL/SCLK/MIPI、曝光/增益/group update约束与Bayer相位。

收到资料后，用 `profile.py` 定义的 JSON 字段导入完整有序 `registers: [[address,value,delay_us], ...]`，`source` 写准确文件版本/页码，`status` 为 `vendor_sequence_supplied`。使用 `build.py --profile <vendor.json> --out <new-directory>` 构建；先复核生成的补丁与时序，再执行 `run_device.py --experimental --hfr` 的有界点亮测试。`compile_only` 合成测试产物被上板工具拒绝。

逆向裁剪路线后续重点是图像与Bayer核验、NV21持续吞吐、MaixCDK API集成及重启恢复。具体已完成项以最新验证记录为准。2026-09-08已新增下游绿色/装甲检测的独立全视野180fps入口，见下节；公共Camera/IVPS消费路径仍需独立验收。

## 全视野180fps业务入口（2026-09-08）

`build_official.py --out <新目录> --business` 构建独立 `business_capture`，复用官方
`official_capture.cpp` 的VIN初始化，链接 `main/src` 中的v0.2检测与异步NV21流水线。
部署用 `run_device.py --driver-build <目录> --official-mode full180 --nv21
--itp-depth 4 --queue-depth 4 --system-media-lib --remote-root /root/os04a10-tests
--business-config config/green_detector_full180.conf`，另加 `--seconds` 控制时长。
5～10秒模式/恢复、30秒 `--business-idle`、120秒 `--business-stress` 依次检查通过后，
才能进行1800秒压力测试。完整命令见[项目README](../../README.md)。

业务模式区别于上述旧RAW探针：帧元数据不累积，`frames.csv`、`vision.csv`、
`motion.csv`、`targets.jsonl` 使用有界异步日志。`progress.jsonl` 也是缓冲追加文件，
不要把文件暂未刷新误认为采集停止。`*.io.json` 记录日志容量、高水位、最大写调用耗时；
写失败或缓冲溢出会终止，不能忽略。`leases.json` 检查含预热帧的DMA取得/归还平衡。
`frames.csv.hold_us` 是生产者分发时间，不是消费者最终释放前的持帧时长。
`business.json` 分开记录最新槽替换、定时主动跳过、退出丢弃与上游序号异常。

用 `analyze_business.py <run目录>` 审计业务频率、接收后源年龄、阶段耗时及资源。
180Hz指采集/预测生成目标；绿灯和装甲调用频率分别报告，无真值不能推算准确率。
未标定时角度/PnP/控制安全位无效，NPU/Pose关闭。退出恢复启动器，原始相机模式用
基线5秒复验，不覆盖系统库、不改自启动。版本、失败记录和验收范围见
[180fps业务报告](../../reports/BUSINESS_FULL180_2026-09-08.md)。
