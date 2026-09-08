# 2026-09-07：从现有 OS04A10 源码推导高速裁剪模式

当前已在 MaixCAM2 上得到 **640×360 RAW、30 分钟平均 359.832fps，647,698 帧无序号异常**。这是中心裁剪模式，覆盖原图宽、高各约24%，未实现厂商2×2 binning模式。第一次长测约32秒发生一次跳号；扩大缓冲后完成30分钟采集。长测主机SSH等待超时，证据随后补取；采集统计与进程退出状态分别记录。

## 可复现构建

在项目根目录执行，使用现有本地 MaixCDK/构建目录：

```bash
python3 -B tools/os04a10_highfps/build.py \
  --recipe tools/os04a10_highfps/crop_360_recipe.json \
  --out .maixpy/crop360-new
python3 -B tools/os04a10_highfps/build_capture.py --driver-build .maixpy/crop360-new
export PATH="$PWD/.maixpy/host-tools/sshpass/usr/bin:$PATH"
python3 -B tools/os04a10_highfps/run_device.py \
  --driver-build .maixpy/crop360-new --crop-probe --seconds 10 --sample-frame
```

默认不保存图像，`--sample-frame` 用于诊断。参数 `--crop-probe` 明确选择推导候选，不伪装成厂商模式表；原来的 vendor profile 校验仍保留。配方固定源文件哈希，SDK 不匹配时拒绝构建。生成的 `driver.patch`、完整独立源码副本、`.so`、寄存器序列、`manifest.json` 和采集程序均在指定目录中。

## 推导依据

原始 SDK 的 `os04a10_settings.h` 含 25 张寄存器表，分析脚本会忽略注释、保留写入顺序，并在 reset 后重新计算最终状态。枚举名不作为实际输出尺寸或帧率的证明。

1. 四 lane Linear30 与 Linear60 表的主要时序差异是 VTS 从 3248 变为 1616，HTS 均为 732；这支持先固定时钟与行长，通过帧长建立分档实验。
2. 另有 640×480 的低速 HDR 表，提供输出尺寸/偏移的结构线索，但没有把它当作可直接套用的 Linear360 表。
3. 基线 PLL2 的读回为 `0322=01, 0323=02, 0324=00, 0325=90, 0328=05, 032a=02, 032f=00`，在 24MHz XCLK 下推导 SCLK=72MHz，匹配实际全幅约 30.28fps。
4. 同型号两 lane Linear60 表使用 `0325=d8`，对应 SCLK=108MHz。该表还有其他差异，因此只迁移倍频字段本身是候选假设；本板已在约 90/180fps 档验证其出帧和时序读回。
5. [OS04A10 rev 1B 规格书](https://dl.sipeed.com/fileList/MaixCAM/Sensors/OS04A10/OS04A10-rev-1B-Preliminary-Specification-Fan-out_Version-1-0_Waching.pdf)第 31、33～41 页提供窗口、HTS/VTS、曝光及 PLL 定义。108MHz 是文档的 SCLK 上限；本试验没有超过它。第 59 页的 360fps binning 能力说明属于另一模式，不能作为本裁剪模式已经正确的依据。

完整表清单及 30→60 差分：`.maixpy/reverse/modes.json`。复现：

```bash
python3 -B tools/os04a10_highfps/reverse_engineer.py \
  <SDK>/dl/extracted/maixcam2_msp_srcs/maixcam2_msp_arm64_glibc_v3.0.0_20250319114413/component/isp_proton/sensor/ov_os04a10/os04a10_settings.h \
  --out .maixpy/reverse/modes.json
```

## 当前候选时序

| 项目 | 数值 / 说明 |
| --- | --- |
| 输出 | 640×360，Linear packed RAW10 |
| XCLK | 24MHz |
| SCLK | 108MHz，沿用基线分频、PLL2 倍频从 0x90 改为 0xd8 |
| HTS / VTS | 732 / 410 |
| 名义 fps | 108,000,000 ÷ 732 ÷ 410 = 359.8560576 |
| 行时间 | 6.77778 微秒 |
| 曝光线数上限 | VTS−8 = 402 行，约 2.724ms；实测结束时 AE 为 401 行 |
| 阵列窗口 | X=0..2703，Y=580..955 |
| 输出偏移 | X=1032，Y=8 |
| 输出覆盖 | 约为全幅图像宽、高的各 24%，面积约 5.6%；未做 binning |
| MIPI | 保持本板已工作的四 lane 接线、PLL1 和 receiver 配置 |

窗口地址对应 `0x3800..0x380b`，输出偏移 `0x3810..0x3813`，HTS/VTS 为 `0x380c..0x380f`，VS HTS 同步到 `0x384c/0x384d`。30分钟RAW测试沿用基线 `RGGB` 声明。最新构建根据基线水平镜像 `0x3820=2`、基线偏移9/9和裁剪偏移1032/8推导为 `BGGR`，记录于manifest的 `bayer_basis`；两次构建的传感器库二进制相同，仅采集端Bayer声明改变。**该相位推导尚未用色卡标定验证**；灰度RAW预览不能代替彩色标定。

## 已有对照结果

除特别注明外均为 5 秒 RAW 短测，传输成功不等于长时/图像质量验收。

| SCLK MHz | HTS | VTS | 水平阵列读出 | 实测 fps / 结果 |
| ---: | ---: | ---: | --- | --- |
| 72 | 732 | 3248 | 656 像素窄窗口 | 30.2816；图像有竖向边界 |
| 72 | 732 | 1616 | 窄窗口 | 60.8629 |
| 72 | 732 | 820 | 窄窗口 | 119.9443 |
| 72 | 732 | 546 | 窄窗口 | 180.1383 |
| 72 | 732 | 3248 | 全宽 | 30.2815；原明显竖向边界消失 |
| 72 | 732 | 410 | 全宽 | 239.8883 |
| 72 | 600 | 400 | 全宽 | 无 RAW；5 次取帧超时后退出 |
| 108 | 732 | 1639 | 全宽 | 90.0130 |
| 108 | 732 | 820 | 全宽 | 179.9435 |
| 108 | 750 | 400 | 全宽 | 无 RAW；5 次取帧超时后退出 |
| 108 | 732 | 410 | 全宽 | **359.8324，10 秒 3598 帧** |

两次无 RAW 的组合都包含 VTS=400，但没有完成保持其他参数完全不变的最小 VTS 边界扫描，因此不能把 VTS=410 宣称为传感器的确定最小值，也不能仅凭这两次失败断言 HTS=600 不支持。当前选择已经实际出帧的参数组合。

10 秒目标短测：[分析](../../.maixpy/runs/os04a10-20260907-213741-ae92b4/analysis.json)、[CSV](../../.maixpy/runs/os04a10-20260907-213741-ae92b4/frames.csv)、[灰度预览](../../.maixpy/runs/os04a10-20260907-213741-ae92b4/preview-gray.png)。无跳号、重复、倒退或取帧/归还错误，温度约 51～55°C，MIPI ErrorCount 为 0。PTS 中位间隔为 2779 个原始单位；单位/相位未获 SDK 文档确认，仍以单调时钟统计采集速率。

## 长测与缓冲修正

第一轮计划 1800 秒，实际在约 32 秒发现一个序号跳变并退出，保存 11581 帧；最后一次取帧间隔 15.755ms，MIPI ErrorCount=0，温度约55°C。此前三个完整 10 秒窗口为 359.9 / 359.8 / 359.9fps。证据：[分析与健康记录](../../.maixpy/runs/os04a10-20260907-213925-dd7895/analysis.json)。这轮长测未通过。

原程序 RAW 队列只有 4 帧，在约360fps时只能容纳约11ms的数据。现将目标模式队列改为16帧，并按640×360分配 private RAW pool，池中额外保留8帧供ISP处理。系统 YUV 池也按实际尺寸配置，以支持后续独立 NV21 测量。这个修改针对消费端停顿；不能据此提前认定所有丢帧问题已经消除。

程序每秒记录温度、MIPI错误和RSS；发生跳号/PTS异常、80°C温度阈值或MIPI错误时退出。80°C是基于手册85°C结温工作上限留出5°C余量的实验停止值。RSS会随保留的逐帧元数据增长，这部分是测量程序的有界内存，不能直接当作驱动内存泄漏。

## 30分钟 RAW 实测结果

记录目录：[os04a10-20260907-214557-2035d1](../../.maixpy/runs/os04a10-20260907-214557-2035d1)。

| 指标 | 实测 |
| --- | --- |
| 帧数 / 尺寸 | 647,698 / 全部640×360 |
| 单调时钟平均帧率 | 359.8321205fps |
| 完整10秒窗口 | 179个，均为359.8或359.9fps |
| 序号丢失 / 重复 / 倒退 | 0 / 0 / 0 |
| PTS不递增 / 取帧错误 / 归还错误 | 0 / 0 / 0 |
| MIPI ErrorCount | 每秒采样最大值0 |
| 内部温度 | 43.62～68.25°C；未经外部温度计校准 |
| 取帧间隔 P50 / P95 / P99 / 最大 | 2.767 / 3.367 / 3.449 / 20.107ms |
| RSS | 22,632～53,028KB，包含有界保留的逐帧元数据 |

最大取帧间隔体现用户态调度停顿，16帧队列在本次测试中保持序号连续。静态场景与序号连续不能替代受控运动场景的逐帧曝光验证。

[完整CSV](../../.maixpy/runs/os04a10-20260907-214557-2035d1/frames.csv)、[分析JSON](../../.maixpy/runs/os04a10-20260907-214557-2035d1/analysis.json)、[健康记录](../../.maixpy/runs/os04a10-20260907-214557-2035d1/health.csv)、[曲线PNG](../../.maixpy/runs/os04a10-20260907-214557-2035d1/validation.png)、[曲线PDF](../../.maixpy/runs/os04a10-20260907-214557-2035d1/validation.pdf)。这些本地原始数据位于Git忽略目录，分享/迁移时需另行保存。

本次实验库SHA256：`07119dee1a09eb8aee86a5ed7b649ddf4f3edc296a0595011dc5b2c382a52395`。完整独立源码与二进制在 `.maixpy/crop-359-buffered/`；最新可复现构建在 `.maixpy/crop360-final/`，其采集程序默认RAW队列16、NV21队列4。

长测 `capture.json` 记录采集退出码0，但主机SSH等待1835秒后超时，未取得最终进程退出码。随后确认板端采集进程已消失、启动器已恢复、原系统库哈希不变，并补取全部证据；`run.json` 明确标记 `ssh_timeout` 和 `recovered_artifacts`。不将采集完成等同于已证明该轮完整析构正常。工具现增加SSH keepalive、板端stdout/stderr文件和 `process_exit_code`，传输失败仍单独保留。

修复后的10秒RAW复测：[os04a10-20260907-222408-d7ccf7](../../.maixpy/runs/os04a10-20260907-222408-d7ccf7/run.json)，3598帧、359.808fps，无序号/取帧/归还错误，板端进程和SSH均返回0，析构日志记录driver released。该短测验证退出流程，不替代重新进行一次完整30分钟退出验证。

## NV21独立短测

首次NV21测试沿用RAW的16帧队列，`AX_VIN_SetChnAttr` 返回 `0x8011010a`（SDK定义为非法参数），未进入采集。仅将队列改为4后，10秒得到3598帧、359.832fps，帧号连续、取帧/归还错误0，进程及SSH返回码0。因此默认NV21队列设为4；这不是对所有可用队列深度上限的完整扫描。

[10秒记录](../../.maixpy/runs/os04a10-20260907-222717-3905e5/analysis.json)、[NV21彩色预览](../../.maixpy/runs/os04a10-20260907-222717-3905e5/preview-nv21.png)。样帧640×360、stride640、345600字节，格式枚举4（NV21）。预览按BT.601有限范围转换，已可辨识场景，未做色卡/ISP标定。RAW的30分钟结论不能直接延伸至NV21。

30秒复测：[分析](../../.maixpy/runs/os04a10-20260907-222820-54f0d8/analysis.json)、[彩色预览](../../.maixpy/runs/os04a10-20260907-222820-54f0d8/preview-nv21.png)。共10,795帧、359.8250678fps，序号/PTS异常、取帧/归还错误及MIPI错误均0，进程与SSH返回0。

复现NV21短测：

```bash
python3 -B tools/os04a10_highfps/run_device.py \
  --driver-build .maixpy/crop360-final --crop-probe --nv21 --seconds 30 --sample-frame
python3 tools/os04a10_highfps/analyze.py .maixpy/runs/<run-directory>
python3 tools/os04a10_highfps/preview_nv21.py .maixpy/runs/<run-directory>
```

最终构建默认参数复测：[os04a10-20260907-222938-0cd014](../../.maixpy/runs/os04a10-20260907-222938-0cd014/run.json)，NV21 5秒1800帧、约359.834fps，进程与SSH返回0。最终构建 `.maixpy/crop360-final/` 提供 `libsns_os04a10.so`、`driver.patch`、完整源码副本、寄存器清单、manifest和独立采集程序；使用版本固定的配方可以重新生成。

最终回退：[os04a10-20260907-223040-e5c170](../../.maixpy/runs/os04a10-20260907-223040-e5c170/run.json)，回调确认加载 `/opt/lib/libsns_os04a10.so`，2688×1520 RAW 5秒151帧、30.2814fps，进程与SSH返回0；启动器恢复，系统库SHA256保持 `25f1d2c4e9fdef213dca3fdecf09f98a61d9a52c0704ade31cf6070aeab5bb4c`。

## 交付边界

目前交付为独立实验驱动与采集工具。RAW已完成上述30分钟采集核验；彩色色卡标定、受控动态场景、NV21持续吞吐、公共MaixCDK Camera API集成、冷机/热机重启和完整异常恢复尚未全部完成。没有将任何高速改动接入 `main/src/` 的绿色检测业务，也没有覆盖板端系统相机库。
