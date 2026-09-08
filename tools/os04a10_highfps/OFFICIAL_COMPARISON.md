# 2026-09-08：Sipeed 官方高速提交与本项目驱动对比

后续实机验证及录像已完成，见 [OFFICIAL_VALIDATION.md](OFFICIAL_VALIDATION.md)。本文中的“未上板”描述保留为当时源码比较阶段的历史范围。

**官方实现的核心增量是可追溯的 2×2 binning 配置：640×360@360fps 使用 binning 加裁剪，比我们已测的纯裁剪保留更多视野；同时提供 1344×760 全视野、最高 180fps 的配置路径。两个提交均未实现全视野 360fps。**

本次完成固定提交的源码核对及主机模拟寄存器写入，没有部署官方版本。官方源码注释中的验证结果属于上游陈述，不等于本项目设备的复测结果。

## 对比对象与证据

| 对象 | 固定版本 / 证据 |
| --- | --- |
| 官方 MaixCDK | [fa498da900ffced8a09d79793b43d07f5bacf65e](https://github.com/sipeed/MaixCDK/commit/fa498da900ffced8a09d79793b43d07f5bacf65e)，父提交 `7d74543adacaed53c928a55e4fb89e28adb6861d` |
| 官方 MSP sensor | [71ca5afe0b7db721c1c95a7488db6d9223060365](https://github.com/sipeed/maix_ax620e_sdk_msp/commit/71ca5afe0b7db721c1c95a7488db6d9223060365)，父提交 `534a7d69f642f5f5fde7f4866691dcca195047de` |
| 我们的纯裁剪驱动 | [crop360-final manifest](../../.maixpy/crop360-final/manifest.json)，库 SHA256 `07119dee1a09eb8aee86a5ed7b649ddf4f3edc296a0595011dc5b2c382a52395` |
| 我们的全幅对照 | [FULL_FOV.md](FULL_FOV.md)，原生 2688×1520、约 89.958fps，未启用 binning |
| 本次源码存档 | [.maixpy/official-comparison](../../.maixpy/official-comparison/)，含提交 JSON、逐文件补丁、固定版本源码及 [下载哈希](../../.maixpy/official-comparison/fetch.json) |

官方 `os04a10_settings.h` 与我们本机旧 MSP 的对应文件 SHA256 相同：`e469a1321125685357202e211cd52c685e92f9929a0de576079c92d01314949f`。因此新能力来自基础模式表之后的动态增量及上层适配，不能只搜索原有 settings 数组判断是否支持高速。

## 核心差异

| 项目 | 我们目前的实现 | 这次官方实现 |
| --- | --- | --- |
| 640×360 采样方式 | 原生像素纯裁剪，没有 binning | 2×2 binning 后再裁剪 |
| 可见区域对应的原生有效像素范围 | 约 640×360 | 约 1280×720 |
| 相对原生 2688×1520 的宽 / 高覆盖 | 约 23.8% / 23.7% | 约 47.6% / 47.4% |
| 全视野路线 | 原生 2688×1520，90fps RAW 已短测 | 1344×760 binning，代码允许最高 180fps；也保留原生 30/60fps |
| 360 档 SCLK / HTS / VTS | 108MHz / 732 / 410 | 相同；代码推导名义帧率均约 359.856fps |
| PLL1 / MIPI 配置 | `0x0305=0x3c`，每 lane 约 720Mbps | `0x0305=0x5c`，每 lane 约 1104Mbps，另有高速伴随寄存器 |
| 驱动组织 | 独立高速枚举、生成完整序列、从最终状态解析 AE | 复用原四 lane Linear60 模式，新增每 pipe crop 状态和运行时 delta |
| 公共 Camera API | 尚未集成；使用独立 RAW/NV21 采集程序 | 集成 `set_windowing`、帧率选择、动态重启与失败恢复 |
| 项目设备实测 | RAW 30 分钟约 359.832fps；NV21 30 秒约 359.825fps | 本轮没有上板测试 |

视野数字是像面有效采样覆盖的近似值，排除保护像素；官方方案约为我们方案的两倍宽、两倍高、四倍面积。**这不表示镜头的角视野翻倍，也不表示恢复了完整视野。** 具体边缘、镜像、畸变及 ISP 输出适配仍需固定机位样图验证。

## 关键寄存器对比

官方入口为 [os04a10_set_crop / os04a10_apply_crop](https://github.com/sipeed/maix_ax620e_sdk_msp/blob/71ca5afe0b7db721c1c95a7488db6d9223060365/component/isp_proton/sensor/ov_os04a10/os04a10.c#L29)。下表按中心 640×360@360 请求比较，数值均为十六进制。

| 寄存器 | 我们的纯裁剪 | 官方 binning + crop |
| --- | --- | --- |
| `0x3714` / `0x37cf` | `01 / 02` | `04 / 04` |
| `0x3814..0x3817` | `01 / 01 / 01 / 01` | `03 / 01 / 03 / 01` |
| `0x3811` / `0x3813`（偏移低字节） | `08 / 08`，水平高字节为 `04` | `05 / 02`；大宽度 binning 的垂直低字节改为 `03` |
| `0x4009` / `0x4051` | `11 / 07` | `07 / 03` |
| `0x4601` / `0x4603` / `0x460c` | `30 / 00 / 60` | `50 / 01 / 50` |
| `0x0305` / `0x0325` | `3c / d8` | `5c / d8` |
| `0x3426` / `0x3427` / `0x3428` | `10 / 14 / 10` | `50 / 15 / 50` |
| `0x4800` | `64` | `44` |

官方配套修改涉及采样、模拟及格式化路径，不能把它缩减为只写 `0x3814=3` 的单寄存器试验。源码注释将配套 delta 归因于公开 Ingenic OS04A10 binning 表，将高速 PLL delta 归因于 Ambarella 90fps 表；本次以这份固定的官方源码为直接依据，未独立核验注释所指的原始表。

官方保留基础表的 `0x3820/0x3821`，在 VIN 中声明 RGGB，并按 binning 宽度调整垂直偏移。我们最新纯裁剪声明 BGGR，由原窗口、镜像与新偏移的奇偶关系推导；两者采样和窗口不同，不能直接认定其中一个 Bayer 声明错误。我们还未做色卡标定。

几何也有实质差别：

- 我们阵列窗口为 X=0..2703、Y=580..955，通过水平输出偏移 1032 得到 640 像素输出；阵列横向跨度大不等于可见视野大。
- 官方中心 ROI 为 `[704,404,640,360]`。x/y 使用原生阵列坐标，w/h 使用 binning 后输出尺寸；带保护像素的物理窗口为 1296×728，终点为 `[1999,1131]`。
- 官方全视野 binning ROI 为 `[0,4,1344,760]`，带保护像素的物理窗口为 2704×1528。这里的“全视野”沿用官方有效成像区域定义，并非保留所有保护行。
- 官方没有显式写偏移高字节 `0x3810/0x3812`，依赖初始化/复位状态；实机对照时应读回，不能把源码未写的寄存器自动记为零。

## 帧率、AE 与采集链路

[官方 AE 代码](https://github.com/sipeed/maix_ax620e_sdk_msp/blob/71ca5afe0b7db721c1c95a7488db6d9223060365/component/isp_proton/sensor/ov_os04a10/os04a10_ae_ctrl.c#L716)先解析旧表，再在高速条件下将 SCLK 修正为 108MHz，并由请求帧率计算 VTS。超过 120fps 的 binning 请求启用高速 PLL。

| 官方请求 | 推导 SCLK | HTS | VTS | 算术名义 fps |
| --- | ---: | ---: | ---: | ---: |
| 全视野 binning 60 | 72MHz | 732 | 1639 | 60.0126 |
| 全视野 binning 180 | 108MHz | 732 | 819 | 180.1477 |
| 640×360 binning crop 240 | 108MHz | 732 | 615 | 239.9040 |
| 640×360 binning crop 360 | 108MHz | 732 | 410 | 359.8561 |

180 档明确将四舍五入得到的 820 改为 819。以上均为源码算式结果，不能替代板端真实帧序号和时间测量。360 档的行时间及 8 行最大曝光余量与我们相同；官方额外校验 `VTS >= 输出高度 + 16`。我们的候选则由配方限制最小 VTS=400。

官方保留慢快门档位并扩展到高速范围；我们针对实验高速枚举禁用了慢快门。复测官方驱动时应记录 AE/慢快门设置，确认暗光下是否改变 VTS；配置为 360 并不自动证明全程固定 360。

官方 [ax_middleware.hpp](https://github.com/sipeed/MaixCDK/blob/fa498da900ffced8a09d79793b43d07f5bacf65e/components/maixcam_lib/include/maixcam2/ax_middleware.hpp#L971)联动 Sensor、Dev、Pipe 和通道尺寸，并对 FBC 通道采用 128 像素 stride 对齐。代码注释说明这用于修复 1344 宽度下 RAW 正常而 YUV 块状损坏的问题。我们的独立 640×360 NV21 使用未压缩通道；RAW 队列 16、NV21 队列 4 是分别实测得到的配置，不能据此断言公共 API 的默认缓冲也能无丢帧。

官方 binning/crop 路径关闭 AI-ISP；我们高速采集程序也关闭。官方还更新普通初始化和 QS 初始化入口，并增加 `-lax_sys`、`--no-undefined` 链接要求。我们的独立构建选择普通驱动对象，不能直接把两套构建脚本视为等价。

错误处理方面，我们对 I²C 初始化、身份检查、模式写入和 AE 初始化失败增加提前返回；官方 `os04a10_init` 中 crop 写入失败记录错误后仍继续，AE 初始化返回值也未在该处检查。官方 Camera 层另外通过预热取帧超时做恢复。整合时可保留我们的底层提前失败检查，并采用官方上层恢复逻辑。

## 应用接口的变化

[官方 Camera 实现](https://github.com/sipeed/MaixCDK/blob/fa498da900ffced8a09d79793b43d07f5bacf65e/components/vision/port/maixcam2/maix_camera_maixcam2.cpp#L719)明确区分应用输出和传感器输入：

- 不指定 ROI 时，小尺寸输出使用固定 1344×760 全视野 binning 输入，再由 VI 适配输出，最高接受 180fps。
- 仅请求 `640×360, fps=360` 会因缺少小 ROI 而被拒绝。需要先设置窗口，再启动相机。
- 显式 2688×1520 输出在不超过 60fps 时走原生输入；超过 60fps 则换成 1344×760 binning 输入再放大，输出尺寸不能作为真实原生分辨率证据。
- `set_fps()` 现在重启并设置 Sensor/VIN 时序；旧接口只调曝光。动态 `set_windowing()` 也重启，预热失败会尝试恢复原配置。
- w 必须为 16 的倍数、x/y 和 h 为偶数；还明确拒绝 `w<1024 && h>464` 这类官方记录的 AX ISP 故障组合。

与官方示例一致的 C++ 调用顺序如下（需配套新 CDK 和新 sensor 库；本段未在本项目上板运行）：

```cpp
maix::camera::Camera cam(640, 360, maix::image::FMT_YVU420SP,
                         nullptr, 360, 3, false);
maix::err::check_raise(cam.set_windowing({640, 360}));
maix::err::check_raise(cam.open());
```

接口通过 `dlsym` 查找新增的 `os04a10_set_crop`，必要时 `dlopen("/opt/lib/libsns_os04a10.so")`。只更新 CDK 而继续加载旧 sensor 库不足以启用此功能；这两个源码提交也不等于用户现有 MaixPy 安装已更新。隔离部署时须核验实际加载对象，不能只依赖设置了 `LD_LIBRARY_PATH`。

[官方 crop 测试例程](https://github.com/sipeed/MaixCDK/blob/fa498da900ffced8a09d79793b43d07f5bacf65e/examples/os04a10_sensor_crop_test/main/src/main.cpp)增加寄存器回读、RAW 单帧和取帧计数；视频例程还增加队列及高帧率封装逻辑。后者按帧索引重新生成 MP4 时间戳，文件标注 360fps 不能作为 sensor 原始采集 360fps 的独立证据。仍应沿用本项目的帧序号、PTS、MIPI/VIN 和长期采集统计。

## 对后续逆向路线的影响

此前“在已检索代码中未找到完整 binning 配置”的结论应限定为当时那组来源。**现在已有官方可追溯的 2×2 配套序列，继续研发应以它为新基线，不再把这组配置当成完全未知的猜测。** 这补上了之前关于传感器内部全视野 binning 能力的关键证据；全视野 skipping 或更高倍降采样仍未解决。

全视野 1344×760 在当前 HTS=732、SCLK=108MHz 下，360fps 需要 VTS≈410，而官方模式最低要求为 760+16=776 行。该组合不满足当前模式约束。不能简单扩大窗口并保留 360 档 VTS，也不能把输出缩成 640×360 认作读出速度提高。

下一步有意义的对照顺序为：官方全视野 binning 60→180、官方中心 binning crop 360、最后再研究未证实的进一步降采样。第一阶段应固定相机与场景，验证视野和 Bayer；再沿用现有 RAW/NV21 统计、长测和回退流程。官方 2×2 配置本身没有证明全视野 360fps 可行或绝对不可能。

## 本次验证范围

提取固定 MSP 提交中的实际 crop C 函数，替换 I²C 为主机内存模拟，使用本机 C 编译器执行六组输入。四组合法模式完成寄存器写入；全视野 360 与 640×480 的明确不支持组合按预期拒绝。AE 的 VTS 另按源码算式计算，未模拟整个 ISP/AE 流程。

复现：`python3 -B .maixpy/official-comparison/replay.py`。结果见 [comparison.json](../../.maixpy/official-comparison/comparison.json)。其中未显式写入的值标为 null，不表示硬件为零；曝光还会受后续 AE 影响。这项检查只核实代码分支和寄存器写入，不是完整官方驱动编译、传感器响应或成像验收。

本轮未修改当前实验驱动、共享 SDK、板端系统库或业务代码；只新增比较记录并更新文档入口。
