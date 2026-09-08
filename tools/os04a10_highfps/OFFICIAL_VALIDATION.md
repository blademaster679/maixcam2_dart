# OS04A10 官方高速功能实机验证 · 2026-09-08

后续[360fps复测](OFFICIAL_RETEST.md)已通过增加ITP内部源队列深度，完成NV21的30分钟无缺号采集；连续硬件编码仍未通过。下面保留初测时的结果及失败证据。

**官方全视野 1344×760@180 的 VIN NV21 采集通过 30 分钟测试；官方 binning + crop 640×360@360 短测达到约 359.83fps，但本次 30 分钟目标测试在约 30.36 秒出现 1 个序号缺口并停止，长期无丢帧未通过。** 已交付两种模式的真实录像。360fps 视频使用一秒无压缩缓存后离线编码，不能据此声称板端能持续实时编码 360fps。

## 视频

| 内容 | 正常速度 | 30fps 慢放 | 保存方式 / 核验 |
| --- | --- | --- | --- |
| 全视野 1344×760，180fps | [10.006秒](../../artifacts/os04a10_official_20260908/full180-realtime.mp4) | [60.033秒，6倍慢放](../../artifacts/os04a10_official_20260908/full180-slow30.mp4) | 1,801帧全部经板端异步 AX VENC 输出；编码帧号、PTS 与采集日志逐项一致 |
| binning + crop 640×360，360fps | [1秒](../../artifacts/os04a10_official_20260908/crop360-realtime.mp4) | [12秒，12倍慢放](../../artifacts/os04a10_official_20260908/crop360-slow30.mp4) | 连续360帧 NV21 缓存，实测359.741fps；电脑端无损 H264，逐帧像素与原始帧相同 |

另提供 [360fps 慢放兼容版](../../artifacts/os04a10_official_20260908/crop360-slow30-compatible.mp4)：H264 High、CRF18有损副本，仍为360帧/12秒；用于常见播放器观看，像素无损证据以表中的原始版本为准。

四个 MP4 均完成整段解码、帧数检查及源图像顺序比较。没有插帧或补帧。180fps 的硬件 H264 本身是有损编码；360fps 的离线 H264 为无损。正常速度/慢放 MP4 按帧索引生成 CFR 时间戳，并同步修改 H264 VUI 时序；原始采集 PTS 保留在 CSV，不能把 MP4 fps 标签用作传感器帧率的独立证据。

[180fps 录像核验](../../artifacts/os04a10_official_20260908/full180-video-validation.json) · [360fps 录像核验](../../artifacts/os04a10_official_20260908/crop360-video-validation.json) · [图像视野对照](../../artifacts/os04a10_official_20260908/fov-comparison.png)。拍摄对象是当前静态桌面，不包含已知速度运动物体或同步光源；慢放可用于逐帧查看，不是受控运动计时试验。

## 固定来源与运行环境

- [MaixCDK fa498da900ffced8a09d79793b43d07f5bacf65e](https://github.com/sipeed/MaixCDK/commit/fa498da900ffced8a09d79793b43d07f5bacf65e)。
- [MSP 71ca5afe0b7db721c1c95a7488db6d9223060365](https://github.com/sipeed/maix_ax620e_sdk_msp/commit/71ca5afe0b7db721c1c95a7488db6d9223060365)。
- 设备：有线 MaixCAM2 / AX630C，OS04A10 chip ID 实读 `0x530441`，I²C 0x36，24MHz XCLK；系统 maixcam2-2026-05-29-maixpy-v4.12.5，MSP `3.0.0_20250319114413`。
- 将上述提交实际改变的 sensor C/头文件、Camera 源文件在独立目录编译，依赖本机与板端匹配的 SDK。不是整套新固件刷写。官方传感器寄存器代码没有再修改；测试中间层仅显式绑定隔离库并关闭 AI-ISP。
- 官方测试 sensor 库 SHA256：`6fa95adebdf0f904fa4ea94bcc8d7079fbd38998f68e1ccadac9a63c11ad9273`。构建记录：[manifest](../../.maixpy/official-build/manifest.json)、[应用构建及来源哈希](../../.maixpy/official-build/app-build.json)、[隔离补丁](../../.maixpy/official-build/harness-isolation.patch)。每次运行的实际二进制哈希记录在各自 run.json。
- 公共 Camera API 必须使用本机固件配套 `/usr/lib/libmaixcam_lib.so.1.2.5`，SHA256 `af9c2d486b374842090fdecc9f3989f9c72b4d768cc5b46d16d92e4e2ba1c615`。最初搬用 SDK 内的另一版媒体库会在 VI::add_channel 崩溃；改为 `--system-media-lib` 后恢复，未修改鉴权或二进制逻辑。

## 分层结果

帧率统一用 `(N−1)/(最后一次−第一次单调时钟取帧时间)`。下表时长为实际帧序列覆盖时间（短测略小于请求时长），不是视频播放时长。跳号统计采用 SDK 提供的 input sequence；PTS 单位和曝光阶段在本机头文件未明确，实测数值约每秒100万 ticks，仅用于连续性与两个时基的一致性检查。

| 测量点 | 实际秒数 | 帧数 | 实测fps | 缺帧序号数 | 退出码 | 原始证据 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 原始驱动基线 | 4.954 | 151 | 30.281 | 未统计 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-131934-3789bf/capture.json) |
| RAW 全视野60 | 4.982 | 300 | 60.017 | 未统计 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-132434-648fc4/capture.json) |
| RAW 全视野120 | 4.994 | 600 | 119.944 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-132539-c56481/capture.json) |
| RAW 全视野180 | 29.993 | 5,404 | 180.140 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-132604-fddb06/capture.json) |
| RAW 裁剪240 | 4.994 | 1,199 | 239.889 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-132654-277808/capture.json) |
| RAW 裁剪360 | 29.997 | 10,795 | 359.832 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-132719-91f593/capture.json) |
| NV21 全视野180短测 | 29.994 | 5,404 | 180.136 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-132809-4855af/capture.json) |
| NV21 裁剪360短测 | 29.998 | 10,795 | 359.830 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-132859-a53333/capture.json) |
| NV21 全视野180长测 | 1799.992 | 324,244 | 180.136 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-135758-4ac11d/capture.json) |
| NV21 裁剪360长测（失败） | 30.360 | 10,921 | 359.681 | 1 | 10 | [记录](../../.maixpy/runs/os04a10-20260908-143631-a69eae/capture.json) |
| Camera全视野180，缓冲8 | 30.000 | 5,398 | 179.903 | 7 | 10 | [记录](../../.maixpy/runs/os04a10-20260908-134818-d2b90a/capture.json) |
| Camera裁剪360，缓冲8 | 30.000 | 10,766 | 358.833 | 30 | 10 | [记录](../../.maixpy/runs/os04a10-20260908-134910-4cde6f/capture.json) |
| Camera切换及重开 | 4.996 | 901 | 180.135 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-135002-b1d499/capture.json) |
| 180fps硬件录像 | 9.993 | 1,801 | 180.130 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-143038-ae4718/capture.json) |
| 360fps原始帧录像 | 0.998 | 360 | 359.741 | 0 | 0 | [记录](../../.maixpy/runs/os04a10-20260908-143959-f2caa3/capture.json) |

180fps 长测共324,244帧，平均180.135760fps，179个完整10秒窗口为180.1～180.2fps，未发现序号重复、逆序、PTS倒退、取帧/归还错误；MIPI ErrorCount 和可见 ErrorStatus 为0。内部传感器温度最高68.7882℃。RSS由约22.0MiB增至36.9MiB，增量与预留容器中逐帧保留元数据的约14.8MiB对应；这不替代多次长测的内存泄漏诊断。[30分钟曲线](../../artifacts/os04a10_official_20260908/full180-30min.png)。主机原会话句柄中断后，板端有界进程继续完成；结束标记为0，补取完整CSV后恢复启动器并核对原库哈希，见该 run.json 的恢复说明。

360fps NV21 长测请求1800秒，在第10,921个保留帧处出现一个缺号，程序返回10并停止。此前三个完整10秒窗口分别359.9、359.8、359.9fps；终点取帧停顿影响了短窗口总平均，不能将359.681fps解释为传感器固定时序变化。HTS/VTS前后均732/410，MIPI计数0。**原因尚未定位，不能把未达到30分钟的记录算成长测通过。** 先前30秒 RAW/NV21 无缺号短测同样保留，不用成功短片掩盖失败长测。

Camera::pop / IVPS 层仍有少量丢帧：缓冲8时，全视野180档30秒缺7帧，360档缺30帧。去掉循环内健康检查后仍分别缺3帧和37帧，说明不是单纯I²C监测造成。IVPS FRC读取为0/0、禁用状态；目前不足以区分调度、队列和上游原因。默认Camera路径没有通过全帧消费验收。

动态 Camera API 检查通过：60→120→180；全视野360请求返回拒绝且保持可用；设置640×360窗口后切到360，再回180/全视野，并连续关闭重开3次。[切换日志](../../.maixpy/runs/os04a10-20260908-135002-b1d499/api_checks.csv)。未进行断电冷启动或硬断连测试。

## 图像、模式与编码限制

全视野模式读回输出1344×760、VTS819，使用2×2 binning；360档读回640×360、VTS410，带binning及中心裁剪，窗口704,404..1999,1131。静态图中640×360区域在全视野样图约(357,198)处无缩放匹配，相关系数0.9252，支持实际视野缩小；不是镜头角视野标定。目视未发现整幅颜色错配、块状损坏或错行，但没有色卡、受控照度和外部测温，不能代替完整画质/温度标定。

公共 Camera + Encoder 初始10秒录像分别只得到505/908个可解码帧，有明显队列丢弃，未作为交付视频。独立 AX VENC 异步方式完整记录180fps；360fps仍出现发送队列满，扩大队列/缩短取流等待的有界对照没有解决。复测核对 SDK 后确认错误 `-2147024349 = 0x80070223 = AX_ERR_VENC_QUEUE_FULL`，此前称为“发送超时”不准确；`AX_ERR_VENC_TIMEOUT` 的错误码低字节为 `0x27`。不存在本次验证过的360fps连续硬件编码承诺。

360fps交付采用预分配375帧、123.6MiB RAM上限的一秒NV21缓存；使用 AX_SYS_MmapCache + MinvalidateCache 保证DMA写入后的CPU读取一致性，在停止采集后落盘，主机无损编码。成功样本进程RSS约145.6MiB。较早无缓存读取版本在复制时落后并跳号，已保留失败记录。此方法只验证短时完整录像，不保证长时间保存能力。

未验证项目：全视野360fps、显示360fps、绿色检测逐帧处理、已知频率光源/运动的独立曝光计时、QS启动、断电冷启动、SoC降频与外部温度。当前 main/src 业务代码及共享 MaixCDK 未改动。

## 复现与源码

在本项目当前构建环境中，使用现有 `.maixpy` 设备配置和凭据。下面命令会自动进入开发模式、独立加载驱动、收集证据并恢复启动器；不覆盖系统库。新构建目录要求为空；来源归档在 `.maixpy/official-comparison`，不是从移动分支拉取。

```bash
export PATH="$PWD/.maixpy/host-tools/sshpass/usr/bin:$PATH"
python3 tools/os04a10_highfps/build_official.py --out .maixpy/official-rebuild
# 全视野180fps连续采集；360档改为 --official-mode crop360
python3 tools/os04a10_highfps/run_device.py --driver-build .maixpy/official-rebuild --official-mode full180 --seconds 1800 --nv21 --system-media-lib --remote-root /root/os04a10-tests --pause-camera-app
# 180fps板端硬件录像
python3 tools/os04a10_highfps/run_device.py --driver-build .maixpy/official-rebuild --official-mode full180 --seconds 10 --direct-venc --record --system-media-lib --remote-root /root/os04a10-tests --pause-camera-app
# 360fps一秒原始帧缓存录像
python3 tools/os04a10_highfps/run_device.py --driver-build .maixpy/official-rebuild --official-mode crop360 --seconds 1 --nv21 --burst --queue-depth 8 --system-media-lib --remote-root /root/os04a10-tests --pause-camera-app
# 将RUN换成命令输出的本地目录，输出目录中不应已有同名视频
python3 tools/os04a10_highfps/analyze.py RUN
python3 tools/os04a10_highfps/package_official_video.py RUN --out OUTPUT
```

核心文件：[构建](build_official.py)、[采集和原始帧缓存](official_capture.cpp)、[Camera接口验证](official_camera_test.cpp)、[硬件录像](official_record.cpp)、[异步编码](direct_venc.hpp)、[部署与恢复](run_device.py)、[视频封装及逐帧验证](package_official_video.py)。最终硬件录像二进制SHA与成功180fps录像时一致。主机13项现有测试通过，Python语法检查和交叉编译通过；这些检查不替代上述板端结果。

## 回退

最终使用起始基线的同一测试二进制，加载 `/opt/lib/libsns_os04a10.so`，完成原生2688×1520、5秒151帧、约30.282fps复测，序号连续且程序返回0。系统库SHA256仍为 `25f1d2c4e9fdef213dca3fdecf09f98a61d9a52c0704ade31cf6070aeab5bb4c`，启动器状态 `daemon=running / launcher=running / autostart=none`。没有覆盖系统库或修改自启动设置。[最终回退记录](../../.maixpy/runs/os04a10-20260908-144219-3ef944/run.json)。较早一次回退调用误选了不支持新参数的旧测试程序，参数解析退出2、没有初始化传感器；随后以上述正确基线完成复测。
