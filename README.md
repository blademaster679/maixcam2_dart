# MaixCAM2 飞镖绿灯识别

这是当前设备端实现：OS04D10 输出 `1280x720 RGB888`，使用 MaixCDK 原生 LAB
阈值与 `Image::find_blobs()` 全图检测，不依赖 OpenCV。程序跟踪单个绿灯，向标准输出逐帧
发送 JSON Lines。

## 已实现的检测链路

- 分别检测绿色光晕和饱和白色圆芯；圆芯必须同时满足周围存在绿色光晕和最小尺寸门限。
- 对候选计算绿色优势、绿像素占比、局部亮度差、密度、长宽比、圆度、核心/光晕偏差、光轴位置先验和时序一致性。
- 关闭全局 Blob 链式合并，避免绿色墙面、地面和机构把灯连成一个大区域。
- 不设置固定最大面积；锁定后使用带上限的位置门控、同源/跨源尺寸门控和最低关联分数拒绝异常测量。
- 远距离使用绿色光晕，近距离优先使用完整的饱和圆芯，避免跟随光晕边缘碎片。
- 六状态卡尔曼滤波器跟踪 `[cx, cy, vx, vy, log_size, size_rate]`。
- 同一空间位置和相近尺寸的候选在最近 5 帧命中至少 3 帧才进入 `TRACKING`；未确认候选不向控制端输出有效坐标，锁定后连续丢失 5 帧回到 `LOST`。
- 仅对输出点做 Brown-Conrady 畸变反解，再由内参计算 yaw/pitch。

## 构建与部署

编译应在 `x86_64` Ubuntu（推荐 Ubuntu 20.04 及以上）或 WSL2 中完成，再把
`AArch64` 程序传到 MaixCAM2。不要把在普通 PC 上原生编译出的 `x86-64` 程序复制到设备运行。
完整的官方流程见 [MaixCDK 快速开始](https://wiki.sipeed.com/maixcdk/doc/) 和
[MaixCDK APP 框架指南](https://github.com/sipeed/MaixCDK/blob/main/docs/doc_zh/convention/app.md)。

### 1. 安装 MaixCDK

已经有可用 MaixCDK 环境时可跳过本节。

```bash
sudo apt update
sudo apt install -y \
    git cmake build-essential \
    python3 python3-pip python3-venv \
    autoconf automake libtool

mkdir -p ~/maix
git clone https://github.com/Sipeed/MaixCDK ~/maix/MaixCDK

python3 -m venv ~/maix/maixcdk-venv
source ~/maix/maixcdk-venv/bin/activate
python -m pip install -U pip
python -m pip install -U -r ~/maix/MaixCDK/requirements.txt

export MAIXCDK_PATH=~/maix/MaixCDK
maixcdk --help
```

每次打开新终端后，需要重新激活 Python 环境并指定 MaixCDK 路径：

```bash
source ~/maix/maixcdk-venv/bin/activate
export MAIXCDK_PATH=~/maix/MaixCDK
```

也可以将上面两行手动加入 `~/.bashrc`，以后打开 Bash 终端时自动加载。

### 2. 固定比赛使用的 MaixCDK 版本

板端系统、MaixPy、MaixCDK 和厂商库可能随版本改变。开发初期可以使用同期最新版；准备比赛
版本时，建议固定与板端 MaixPy 对应的 MaixCDK commit，避免重新编译时出现 API/ABI 不兼容
或行为变化。

在 MaixCAM2 终端查询 MaixPy 版本：

```bash
pip show MaixPy
```

然后在对应的 [MaixPy Release](https://github.com/sipeed/MaixPy/releases) 中查看
`maixcdk_version_*.txt`，在电脑上切换到文件指定的 commit：

```bash
git -C ~/maix/MaixCDK checkout <官方指定的 commit>
```

升级板端系统或 MaixPy 后，应重新确认对应的 MaixCDK commit。正式参赛时建议同时记录板端
系统版本、MaixPy 版本、MaixCDK commit、本工程版本和配置文件版本。

### 3. 编译程序

进入本项目根目录，并确认上一节的虚拟环境和 `MAIXCDK_PATH` 已生效：

```bash
cd /path/to/maixcam2_dart
maixcdk build -p maixcam2
```

第一次编译会下载 MaixCAM2 交叉编译工具链。成功后主要产物为：

```text
build/dart_green_detect    AArch64 可执行文件
build/dl_lib/              随程序部署的动态库
```

可以检查目标架构和文件是否存在：

```bash
file build/dart_green_detect
ls -lh build/dart_green_detect build/dl_lib
```

`file` 的结果应包含 `ARM aarch64`，不应是 `x86-64`。仅修改已有源文件时可使用增量编译：

```bash
maixcdk build2
```

增加或删除源文件后，必须重新执行完整的 `maixcdk build -p maixcam2`。编译异常时可以使用：

```bash
maixcdk build --verbose -p maixcam2
```

必要时清理后重新编译：

```bash
maixcdk distclean
maixcdk build -p maixcam2
```

### 4. 连接设备并确认相机

在 MaixCAM2 的“设置 -> 设备信息”中查看 IP。下面以 `192.168.1.123` 为例，执行前替换为
实际地址：

```bash
export MAIXCAM2_HOST=192.168.1.123
ssh root@"$MAIXCAM2_HOST"
```

MaixCAM2 默认 SSH 用户名为 `root`，密码为 `sipeed`。在板端查询当前相机 Sensor：

```bash
python3 -c 'from maix import camera; print(camera.get_device_name())'
```

本工程当前按 OS04D10 的 `1280x720 RGB888 @ 60fps` 编写。如果 Sensor 不是 OS04D10，需先
确认其是否支持该模式，并重新标定曝光、白平衡、LAB 阈值和相机内参。

### 5. 快速上传并通过 SSH 调试

以下命令在电脑的项目根目录执行：

```bash
ssh root@"$MAIXCAM2_HOST" 'mkdir -p /root/dart_green_detect'

scp build/dart_green_detect \
    config/green_detector.conf \
    main.sh \
    root@"$MAIXCAM2_HOST":/root/dart_green_detect/

scp -r build/dl_lib \
    root@"$MAIXCAM2_HOST":/root/dart_green_detect/
```

相机不能同时被 Launcher 或其它应用占用。可以先连接 MaixVision，让 Launcher 自动退出；
也可以 SSH 登录设备后停止它：

```bash
ssh root@"$MAIXCAM2_HOST"
killall launcher_daemon
```

在板端运行程序：

```bash
cd /root/dart_green_detect
chmod +x dart_green_detect main.sh
./main.sh
```

程序不打开窗口。成功启动后，标准输出会逐帧打印检测 JSONL；配置警告与错误写到标准错误。
按 `Ctrl+C` 停止。需要分别保存结果和日志时使用：

```bash
./main.sh > detections.jsonl 2> detector.log
```

`main.sh` 会定位应用自身目录，将应用的 `dl_lib`、MaixCAM2 平台库目录 `/opt/lib` 和系统原有
动态库路径合并到 `LD_LIBRARY_PATH`，然后使用配置文件的绝对路径启动程序。不要再使用
`LD_LIBRARY_PATH=./dl_lib ./dart_green_detect`：该写法会覆盖系统原有路径，导致
`libax_sys.so` 等 MaixCAM2 平台库无法被加载。

### 6. 打包并安装为 MaixCAM2 APP

SSH 调试通过后，在电脑的项目根目录生成正式安装包：

```bash
maixcdk release -p maixcam2
ls -lh dist/*.zip
```

当前 `app.yaml` 的版本为 `0.1.4`，预计产物为
`dist/dart_green_detect_v0.1.4.zip`。`app.yaml` 会将
`config/green_detector.conf` 安装为应用根目录下的 `green_detector.conf`，并包含本工程维护的
`main.sh`；发布工具会加入可执行文件和 `dl_lib`。本工程显式提供启动脚本，不依赖不同版本
MaixCDK/maixtool 是否自动生成脚本。

可以在上传前检查安装包内容：

```bash
unzip -l dist/dart_green_detect_v0.1.4.zip
```

其中应包含 `main.sh`、`dart_green_detect`、`green_detector.conf` 和 `dl_lib/`。

在电脑上上传安装包：

```bash
scp dist/dart_green_detect_v0.1.4.zip root@"$MAIXCAM2_HOST":/root/
```

在 MaixCAM2 上安装：

```bash
/maixapp/apps/app_store/app_store install \
    /root/dart_green_detect_v0.1.4.zip
```

安装完成后，可从设备应用菜单启动，也可以在 SSH 终端验证：

```bash
cd /maixapp/apps/dart_green_detect
sh ./main.sh
```

Launcher 会按照官方约定使用 `sh` 执行 `main.sh`，不依赖压缩包是否保留脚本的可执行权限。
`main.sh` 还会恢复发布工具可能丢失的二进制可执行权限。正式安装后优先通过 `main.sh` 启动，
因为它会自动配置 `LD_LIBRARY_PATH`。也可以在电脑项目
目录执行 `maixcdk deploy -p maixcam2` 生成二维码，再使用设备的应用商店扫码安装。

如需开机自动运行，在设备中选择“设置 -> Boot Startup -> Dart Green Light Detector”。

### 常见部署问题

- `maixcdk: command not found`：重新激活 `maixcdk-venv`，并设置 `MAIXCDK_PATH`。
- 相机打开失败或提示资源占用：确认 Launcher 和其它相机应用已经退出。
- `error while loading shared libraries: libax_sys.so`：执行
  `find /opt /usr /lib -name 'libax_sys.so*' 2>/dev/null`。如果 `/opt/lib` 中存在该库，通过
  `main.sh` 启动；如果完全不存在，应让板端系统、MaixPy 与 MaixCDK 版本匹配，不要只复制
  单个 `libax_sys.so`。
- 其它 `error while loading shared libraries`：确认已上传完整的 `build/dl_lib`，并通过
  `main.sh` 启动。
- 找不到 `green_detector.conf`：从包含配置文件的应用目录运行，或向 `--config` 传入绝对路径。
- `camera did not accept the configured resolution`：检查 Sensor、板端系统版本以及
  `1280x720@60fps` 模式是否匹配。

## 输出格式

每帧输出一行：

```json
{"timestamp_us":123456,"valid":true,"state":"TRACKING","center_x":641.2,"center_y":359.7,"bbox_x":635,"bbox_y":354,"bbox_w":13,"bbox_h":12,"apparent_size":12.49,"yaw_rad":0.000293,"pitch_rad":0.000073,"confidence":0.91}
```

`valid=false` 表示该帧没有可供控制端使用的确认观测；中心、外接框、角度和置信度均为零。
`CANDIDATE` 表示检测到了尚未完成 3/5 帧同目标确认的候选，因此同样保持 `valid=false`。
锁定期间若测量不满足位置、尺寸或关联分数硬门控，该帧也输出 `valid=false`，卡尔曼状态只预测不更新。

## 上板前必须标定

[默认配置](config/green_detector.conf) 中的 LAB、候选评分和时序门控参数已用当前五段视频调节，
其中三个新样本覆盖地面反光、背景海报和零散小亮点干扰；它们尚未经过纯负样本和完整比赛距离
逐帧标注验证。曝光、增益、白平衡和内参仍是启动模板。尤其是
`camera.exposure_us=0`、`camera.gain=-1` 和 `camera.manual_white_balance=false` 会保留自动模式，
只适合采集标定数据；程序会在标准错误中给出警告。

建议先固定镜头与光圈，然后完成以下步骤：

1. 在 `0.5、1、3、5、10、15 m` 采集目标与干扰物，覆盖暗场、室内灯、反光和运动模糊。
2. 从远距离灯芯与近距离光晕分别统计 LAB 范围，更新 `lab.core` 和 `lab.halo`。
3. 逐步缩短曝光，选择既不让近距离光斑完全失真、又能保留 15 m 灯点的固定曝光与增益。
4. 固定白平衡增益并重新采集一次；自动曝光或白平衡开启时得到的阈值不能直接用于比赛。
5. 用棋盘格求 `fx/fy/principal_x/principal_y/k1/k2/p1/p2/k3`，不要仅由标称视场角估算。

调试时可开启 `debug.enabled`。保存项包含未绘制的 JPEG 原图和同名 JSON 候选信息；
`debug.max_saved_frames` 用于限制写盘数量。正式运行应关闭调试。

## 主机侧测试

测试只编译平台无关的配置、角度和跟踪核心，不需要 MaixCDK 或相机：

```bash
cmake -S tests -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

在包含中文路径的 Windows 工作区中，MinGW Makefiles 可能无法创建对象文件；可将第一条命令
改为 `cmake -S tests -B build-host -G Ninja`。

这些测试不能替代真实灯光数据验收。98% 召回率、5 像素中心误差 P95 和 `0.15 deg`
角误差 P95 需要使用人工标注的数据集另行测量。

## 实拍视频离线回放与可视化

`tools/video_replay` 提供主机侧 OpenCV 回放工具。它复用正式程序的配置解析、候选评分、
时序关联和卡尔曼跟踪代码，仅使用 OpenCV 适配 MaixCDK 的 LAB `find_blobs()`。因此它适合
快速调参和检查漏检区间，但 OpenCV 与 MaixCDK 的 Blob 轮廓、合并和圆度实现存在少量差异，
最终阈值仍需在 MaixCAM2 实机上确认。

Ubuntu/WSL2 安装依赖并编译：

```bash
sudo apt update
sudo apt install -y libopencv-dev ffmpeg

cmake -S tools/video_replay -B build-video-replay -DCMAKE_BUILD_TYPE=Release
cmake --build build-video-replay -j
```

处理当前五段视频（旧的 `0/1.mp4` 和新录制的 `2/3/4.mp4`）：

```bash
mkdir -p recordings/maixcam2/2026-07-03/results_v0.1.4

for id in 0 1; do
    build-video-replay/dart_video_replay \
        --input "recordings/maixcam2/2026-07-03/${id}.mp4" \
        --output "recordings/maixcam2/2026-07-03/results_v0.1.4/${id}_detected.mp4" \
        --jsonl "recordings/maixcam2/2026-07-03/results_v0.1.4/${id}_detections.jsonl" \
        --config config/green_detector.conf
done

mkdir -p recordings/maixcam2/2026-07-03/new_2026-08-16/results
for id in 2 3 4; do
    build-video-replay/dart_video_replay \
        --input "recordings/maixcam2/2026-07-03/new_2026-08-16/${id}.mp4" \
        --output "recordings/maixcam2/2026-07-03/new_2026-08-16/results/${id}_detected_v0.1.4.mp4" \
        --jsonl "recordings/maixcam2/2026-07-03/new_2026-08-16/results/${id}_detections_v0.1.4.jsonl" \
        --config config/green_detector.conf
done
```

视频中会绘制全部候选框、最终检测框、滤波后中心、跟踪状态、置信度、候选数量、单帧耗时、
平滑检测 FPS 和累计平均检测 FPS。输出视频保持原视频分辨率、播放帧率、时长和帧数；显示的
`Detector FPS` 是 x86 主机上的 OpenCV 适配层加检测器耗时，不代表 MaixCAM2 实机帧率。
逐帧 JSONL 可用于进一步统计或与人工标注对比。

当前版本还针对地面反光、海报和小亮点误锁增加了四层保护：只有同一物理候选才能累计 3/5 帧
确认；白色圆芯周围必须有足够比例的绿色像素；已锁定目标使用有上限的位置/尺寸硬门控；中心和
初始尺寸先验只在捕获阶段生效，避免锁定后被背景高分候选拉走。默认配置使用以光轴为中心、半径
`120 px` 的软先验；这符合当前相机正对飞镖架的安装方式。如果目标允许长期偏离画面中心，应
增大 `detector.center_prior_radius_px` 或降低 `score.weight_center_prior`。

同一回放工具下，旧结果与当前默认配置的结果如下：

| 输入 | 配置 | 分辨率 | 总帧数 | 有效观测帧 | `TRACKING` 帧 | 主机平均检测 FPS |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| `0.mp4` | 改进前 | 1920x1080 | 353 | 44（12.5%） | 46（13.0%） | 1.714 |
| `0.mp4` | v0.1.4 | 1920x1080 | 353 | 350（99.2%） | 351（99.4%） | 66.272 |
| `1.mp4` | 改进前 | 1280x720 | 295 | 27（9.2%） | 29（9.8%） | 12.531 |
| `1.mp4` | v0.1.4 | 1280x720 | 295 | 292（99.0%） | 293（99.3%） | 124.626 |

抽样检查中，检测框从远距离小灯点连续增长到近距离饱和圆芯，并保持在圆芯中心。上述比例只
表示检测器在两段“绿灯始终存在”的正样本中有输出，不是召回率或准确率；尚未逐帧人工标注，
也没有纯负样本可测量误检率。三个带干扰的新视频的详细对比见
[`TEST_REPORT_v0.1.4.md`](recordings/maixcam2/2026-07-03/new_2026-08-16/TEST_REPORT_v0.1.4.md)。
`0.mp4` 不是程序当前配置的 `1280x720` 采集模式，回放工具会按
分辨率同步缩放内参、空间门限和卡尔曼位置噪声。最终仍需在 MaixCAM2 实机上确认 MaixCDK
Blob 行为和帧率，并用灯灭、其它绿色物体、反光、运动模糊及完整比赛距离的视频继续验收。
