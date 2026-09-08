# 2026-09-07 板端验证记录

结论：**默认模式与可恢复的开发链路通过短测；640×360@360fps 尚未完成。** 没有向设备部署合成高速表，也没有尝试未知 PLL/binning 设置。

## 环境

- 有线 SSH 设备：MaixCAM2，`maixcam2-9d05`，AX630C；OS04A10 chip ID 读回 `0x530441`。
- 系统：`maixcam2-2026-05-29-maixpy-v4.12.5`；MSP：`3.0.0_20250319114413`。
- MaixCDK：`2a0502ecb20e5695b28580b3689492b7a228f9e4`，共享 SDK 工作区检查无修改。
- 工具链：Arm GNU 11.3.rel1，AArch64 Linux glibc。
- 原厂 `/opt/lib/libsns_os04a10.so` SHA256：`25f1d2c4e9fdef213dca3fdecf09f98a61d9a52c0704ade31cf6070aeab5bb4c`。各轮结束后复核一致。
- 实验源码、驱动、构建命令与补丁：`.maixpy/os04a10-build-v3/`。未修改主业务的 `main/src/`。

## 实测

均为 2688×1520、VIN IFE packed RAW10，预热 2 秒后测量，关闭测试进程的 AI-ISP。

| 驱动 | 测量窗口 | 有效帧 | 单调时钟 fps | RAW 序号缺失/重复/倒退 | 取帧/归还错误 |
| --- | ---: | ---: | ---: | --- | --- |
| 原厂初始基线 | 10 秒 | 302 | 30.281547 | 0 / 0 / 0 | 0 / 0 |
| 实验驱动首次短测 | 10 秒 | 302 | 30.281492 | 0 / 0 / 0 | 0 / 0 |
| 实验驱动（明确核对回调库） | 30 秒 | 908 | 30.281430 | 0 / 0 / 0 | 0 / 0 |
| 原厂回退复测 | 10 秒 | 302 | 30.281449 | 0 / 0 / 0 | 0 / 0 |

30 秒实验的帧间隔 P50/P95/P99/最大为 33021 / 33143.4 / 33712.44 / 34231 微秒。RAW 序号连续，PTS 无非递增值。PTS 增量与单调时钟相差表现为约 1MHz 的计数，但 SDK 未注明单位及采样时刻，因此没有把此推断标为经过文档核验的硬件 fps。

MIPI 前后快照中 ErrorStatus0/1、ErrorCount 均为 0；VIN IFE 的 OutFrmDrop/LostCnt 为 0。未消费的 ISP 通道有输出丢弃，不属于所测 RAW 点的丢帧。未获得分项 CRC/ECC 计数与温度记录。

实验回调实际来自 `/tmp/os04a10-20260907-194127-c1a853/libsns_os04a10.so`；回退回调来自 `/opt/lib/libsns_os04a10.so`，均通过 `dladdr` 核实。结束后的 launcher 与 daemon 均 running，autostart 为 none。

实验/回退各保存一帧 5,107,200 字节 RAW 与对应元数据。尚未完成解包显示、Bayer 相位、受控动态画面核验；不能把这些短测称作图像质量或长时稳定性验收。

## 原始证据

日志均在项目忽略的 `.maixpy/` 中，未纳入版本控制：

- [原厂初始基线](../../.maixpy/runs/os04a10-20260907-193752-444a5b/analysis.json)
- [实验驱动首次短测](../../.maixpy/runs/os04a10-20260907-193905-1351c8/analysis.json)
- [实验驱动 30 秒分析](../../.maixpy/runs/os04a10-20260907-194127-c1a853/analysis.json)、[原始帧记录](../../.maixpy/runs/os04a10-20260907-194127-c1a853/frames.csv)、[采集日志](../../.maixpy/runs/os04a10-20260907-194127-c1a853/capture.log)
- [原厂回退复测](../../.maixpy/runs/os04a10-20260907-194424-bedfde/analysis.json)、[恢复状态](../../.maixpy/runs/os04a10-20260907-194424-bedfde/mode-after.log)

每个目录另含 `capture.json`、库映射、MIPI/VIN 前后状态、库哈希和 runner 配置；抽样轮次另含 `sample.raw`/`sample.json`。

## 软件检查与剩余项

6 项主机测试通过，覆盖缺表拒绝、重置后的最终寄存器状态、非法时序/曝光/PLL/stream-on、高速头文件分层及丢帧/重复/逆序检测。默认与启用高速条件编译的 `.so`/采集程序均完成交叉编译。

启用高速条件编译使用 **仅用于编译的合成算术数据**，不是厂商模式表；产物位于 `.maixpy/os04a10-compile-only*`，manifest 标记 `compile_only`，上板工具拒绝部署。这项检查只证明代码分支能编译。

目标 360fps、真实高速图像、NV21、公共 Camera API、30 分钟稳定性、温升、冷机/热机和异常断连恢复均未验收。下一步依赖完整厂商序列与时序约束，详见 [README](README.md)。
