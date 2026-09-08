# 2026-09-08：OS04A10全视野高速模式尝试

后续证据更新：用户提供的 Sipeed 官方提交已包含 2×2 binning 配套配置和 1344×760 全视野最高 180fps 路径，详见 [官方代码对比](OFFICIAL_COMPARISON.md)。下文“未找到 binning 序列”仅描述此前检索范围；不再作为当前资料缺口。全视野 360fps 仍未实现。

**本轮尚未实现不裁剪的640×360@360fps。已经实现并上板验证原生2688×1520、约89.958fps的完整视野RAW对照。** 不能将这项对照称为全视野360fps驱动，也不能把主机缩图当作传感器降采样。

## 本轮查了哪些资料和代码

复用本地OS04A10 rev1B完整规格书，并下载、分析以下同型号公开实现。源码和二进制仅保存在Git忽略的 `.maixpy/fullfov/sources/`；没有运行下载的库，也没有将其他平台的ABI直接用于MaixCAM2。

| 来源 | 提取序列数 | 结果 |
| --- | ---: | --- |
| 本机Axera MSP 3.0.0的 `os04a10_settings.h` | 25 | 含全幅、HDR及640×480序列，没有全视野binning模式 |
| [Rockchip Linux develop-4.19](https://github.com/rockchip-linux/kernel/blob/develop-4.19/drivers/media/i2c/os04a10.c) | 6 | 含公共初始化与局部模式增量 |
| [Rockchip Linux develop-5.10](https://github.com/rockchip-linux/kernel/blob/develop-5.10/drivers/media/i2c/os04a10.c) | 9 | 未找到目标降采样模式 |
| [Rockchip Linux develop-6.1](https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/media/i2c/os04a10.c) | 9 | 未找到目标降采样模式 |
| [Luckfox Pico](https://github.com/LuckfoxTECH/luckfox-pico/blob/main/sysdrv/source/kernel/drivers/media/i2c/os04a10.c) | 9 | 与Rockchip来源有重叠 |
| [OpenIPC / Ingenic T41](https://github.com/OpenIPC/openingenic/blob/master/kernel/sensors/t41/os04a10/os04a10.c) | 2 | 2688×1520和2560×1440 |
| 本机Sophgo SG200x `os04a10_sensor_ctl.c` | 3 | 包含命名与实际输出不完全一致的低分辨率初始化；不能按函数名判定采样方式 |
| [Axera公开AX620E BSP静态库](https://github.com/AXERA-TECH/ax620e_bsp_sdk/blob/main/msp/out/arm64_glibc/lib/libsns_os04a10.a) | 13 | 从ELF64对象的具名数组提取；没有发现额外binning序列 |

合计76段，包括公共初始化、局部增量和跨仓库重复，并非76种独立可用模式。涉及 `0x3814..0x3817` 的显式写入均为1；`0x3820..0x3822`也已列入清单，但不能单凭地址和相似型号习惯赋予未知位binning语义。

另检查了Axera AX620E、AX650N公开仓库文件索引和应用层OS04A10配置。AX650N索引没有提供可复用的OS04A10传感器寄存器源码。网页搜索得到的经销商推测性说明没有作为寄存器依据。

可复现分析器：[source_inventory.py](source_inventory.py)。来源URL、哈希、输出尺寸及采样字段摘要：[fullfov_sources.json](fullfov_sources.json)。详细序列清单：[本地分析JSON](../../.maixpy/fullfov/source_inventory.json)。局部表的缺字段标为未知，不能当作0。

## 为什么没有直接试写binning位

[OS04A10规格书](https://dl.sipeed.com/fileList/MaixCAM/Sensors/OS04A10/OS04A10-rev-1B-Preliminary-Specification-Fan-out_Version-1-0_Waching.pdf)第59页将640×360@360fps标为“2×2 binning and cropping”，更小模式还使用skipping；该表没有列出完整视野360fps组合。第35页窗口寄存器说明止于0x3813，未给出0x3814..0x3817的有效采样取值和配套逻辑。第59页还说明模式切换需要完整配置多个逻辑块的写入序列。

因此，猜测0x3814写3或7、或套用OS04D10/OS04C10的未知控制位，不能构成一个有同型号依据的完整候选。本轮没有进行这种寄存器扫描。Skill的对应约束是“只为有时序依据的模式生成寄存器修改”，见本次读取的 [SKILL.md](/home/blade_master/.codex/skills/maixcam2-os04a10-highfps/SKILL.md)（[项目内副本](../../skills/maixcam2-os04a10-highfps/SKILL.md)）。这不是需要用户再次批准的常规部署步骤，而是目前缺少模式定义的技术依赖。

## 有依据的全幅时序对照

新增 [fullfov.py](fullfov.py)，固定已核对的Axera源文件SHA256。三档均保留原厂全部窗口、偏移、采样、镜像和Bayer声明，仅允许时钟、VTS和初始曝光相关改动。

| 参数 | 30档 | 60档 | 90档 |
| --- | ---: | ---: | ---: |
| 原生输出 | 2688×1520 | 2688×1520 | 2688×1520 |
| 阵列窗口 | X=0..2703，Y=0..1535 | 同左 | 同左 |
| 输出偏移 | 9 / 9 | 同左 | 同左 |
| SCLK | 72MHz | 72MHz | 108MHz |
| HTS / VTS | 732 / 3248 | 732 / 1640 | 732 / 1640 |
| 名义fps | 30.28345 | 59.97601 | 89.96401 |
| PLL1 `0x0305` | 0x3c | 0x3c | 0x5c |
| PLL2 `0x0325` | 0x90 | 0x90 | 0xd8 |
| MIPI每lane时钟推导 | 720Mbps | 720Mbps | 1104Mbps |

90档的PLL2来自同型号已有108MHz模式；PLL1=0x5c来自同型号四lane HDR初始化，核对PLL伴随字段和0x4837保持一致后转用于Linear对照，HDR本身保持关闭。接收器仍保留本机原工作配置，是否正常以板端结果核验。

全幅RAW10约90fps的有效负载超过3.67Gbit/s；4×720Mbps只有2.88Gbit/s，不能承载。规格书第34/59页90fps与720Mbps/lane的摘要组合存在此吞吐矛盾，不能照抄。实际候选采用手册第40页上限及同型号代码均有依据的4×1104Mbps。读回0x0303=2、0x0304=0、0x0305=0x5c、0x0306=0、0x0307=0、0x030c=0，与24MHz XCLK下的推导一致；这不是独立示波器链路测量。

VTS=1640是保守对照值，不是测出的最小帧长。全幅360fps即使只计有效RAW10也需14.709Gbit/s，因此完整像素读出再缩放这条路线本身不成立；要360fps必须在传感器读出环节减少采样。

## 实机结果

| 测试 | 帧数 | 实测fps | 序号异常 / 取帧 / 归还 / MIPI错误 |
| --- | ---: | ---: | --- |
| 全幅30档，5秒 | 151 | 30.28144 | 均0 |
| 全幅60档，5秒 | 300 | 59.97121 | 均0 |
| 全幅90档，5秒 | 450 | 89.95476 | 均0 |
| 全幅90档，30秒 | 2699 | **89.95783** | 均0 |

30秒记录：[analysis.json](../../.maixpy/runs/os04a10-20260908-112247-efd79f/analysis.json)、[frames.csv](../../.maixpy/runs/os04a10-20260908-112247-efd79f/frames.csv)、[寄存器读回](../../.maixpy/runs/os04a10-20260908-112247-efd79f/registers_after.csv)、[完整视野灰度样图](../../.maixpy/runs/os04a10-20260908-112247-efd79f/preview-gray.png)。PTS严格递增，取帧间隔P99约11.838ms，最大15.388ms；内部温度最后读回约56.20°C。进程和SSH返回0。30fps与90fps样图保留同一场景边缘，没有采用新增中心窗口。

本轮均为独立VIN IFE RAW验证，未完成全幅90fps的30分钟长测、NV21或公共Camera API集成。上述89.958fps不能作为360fps验收通过。

## 构建与运行

```bash
python3 -B tools/os04a10_highfps/build.py --fullfov-probe 90 --out .maixpy/native90-new
python3 -B tools/os04a10_highfps/build_capture.py --driver-build .maixpy/native90-new
export PATH="$PWD/.maixpy/host-tools/sshpass/usr/bin:$PATH"
python3 -B tools/os04a10_highfps/run_device.py \
  --driver-build .maixpy/native90-new --fullfov-probe --seconds 10 --sample-frame
```

`--fullfov-probe`与`--crop-probe`互斥，并校验构建manifest；全幅保留原厂池大小，队列限定4帧。实验枚举的旧名称 `e_OS04A10_640x360_HFR` 为兼容现有补丁保留，实际尺寸由生成头文件指定；全幅只有匹配名义fps的请求进入该模式。

30秒验证构建：[manifest](../../.maixpy/fullfov/native90-readback/manifest.json)、[driver.patch](../../.maixpy/fullfov/native90-readback/driver.patch)、[libsns_os04a10.so](../../.maixpy/fullfov/native90-readback/libsns_os04a10.so)。下载的其他平台库仅供离线分析；上板库始终从本机匹配MSP的源码构建。

初次测试检测到官方camera应用占用并拒绝启动；随后真正运行时占用已解除，`paused_official_camera=false`。runner增加了显式 `--pause-camera-app`，只允许暂停官方camera，并通过与 `maix::app::switch_app` 相同的 `/tmp/run_app.txt` 请求恢复；本轮未实际触发该分支。没有修改持久自启动设置。

## 全视野360fps仍需解决什么

需要同型号的完整binning／跳采样寄存器序列、合法组合及最短行/帧时间。一个数学上的研究目标是全幅有效4倍降采样到约672×380，再缩放或留边到640×360，但没有证据证明OS04A10提供该全幅读出模式，不能将其标为可部署配方。

取得具体序列后，先以低帧率验证整个视野、各边缘和Bayer，再验证ADC实际读出时间、MIPI与帧号，最后提升至360fps。现有采集工具、回读和错误统计可复用。当前资料不足以继续生成可靠的binning寄存器写入，不能以扩大PLL或减小VTS代替所缺的降采样模式。

本轮运行的13项主机测试全部通过（其中新增3项），覆盖原有校验、全幅改动白名单/带宽边界、C序列解析及ELF数组布局；共享MaixCDK源码和 `main/src/` 未改。原裁剪模式回归：[记录](../../.maixpy/runs/os04a10-20260908-112425-e92326/run.json)，5秒1799帧、约359.786fps，正常退出。

最终原厂回退：[os04a10-20260908-112758-6e6715](../../.maixpy/runs/os04a10-20260908-112758-6e6715/run.json)，实际回调 `/opt/lib/libsns_os04a10.so`，5秒151帧、30.2818fps，进程和SSH返回0；启动器与daemon恢复运行，系统库SHA256保持 `25f1d2c4e9fdef213dca3fdecf09f98a61d9a52c0704ade31cf6070aeab5bb4c`。
