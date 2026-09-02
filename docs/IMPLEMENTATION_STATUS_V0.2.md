# v0.2 计划落实与验收状态

检查日期：2026-09-01。这里把“代码已实现”“工具链已实现”和“必须由真实数据/实机完成”
分开记录。缺少模型或真值时保持 fail-closed，不用占位数值制造通过结果。

## 落实矩阵

| 计划项 | 状态 | 实现或证据 |
| --- | --- | --- |
| 480×360@60、固定曝光/WB | 185 分钟帧率实测通过，标定待办 | 60.001 FPS、总处理 P95 17.37 ms；500 μs/手动 WB，曝光扫描仍待完成 |
| 归一化绿色响应和 2/4/6/9/14 px 多尺度 | 已实现/已测试 | tile 稀疏候选、定点 RGB 响应；5 px 合成目标及暗部编码噪声测试 |
| 最多 5 候选、±6.5° 捕获锥、取消中心软先验 | 已实现/已测试 | 全图 LAB 可记录锥外候选但不能控制；归一化密集搜索限制在控制锥；中心权重为 0 |
| 60 FPS CPU 调度优化 | 已实现/长时板测通过 | 480×360、传统/运动补偿 30 Hz、控制 60 Hz；185 min 为 60.001 FPS、总 P95 17.37 ms、估算丢帧率 0.0072% |
| 亚像素中心 | 已实现 | 多尺度响应加权中心与 Blob 几何/质心融合 |
| 红/蓝任意滚转双灯条 | 已实现/已测试 | PCA 主轴、平行/长度/色彩一致性/间距/绿灯固定关系；45° 合成测试 |
| 五关键点和装甲中心 | 已实现/已测试 | 统一点序；传统几何和 NPU 输出均映射到同一结构 |
| 平面 PnP、畸变和退化判定 | 已实现/已测试 | 完整端点、最小分离度、Brown 畸变、重投影误差；正面距离/退化测试 |
| 3 次有效后 100 ms 平滑切换 | 已实现/已测试 | `LAMP_APPROACH/FUSED/ARMOR_IMPACT` |
| YOLO11n-Pose ROI 验证 | 运行时代码和工具链完成；权重待训练 | 64–384 ROI、192/256/320 配置；启用后与传统检测隔帧交错，各 30 Hz；三候选轮询、空间一致性、fail-closed |
| 固定输入 INT8 AX620E/MUD | 转换工具完成；产物待生成 | ONNX 导出、100 图选择、Pulsar2 NPU2/NPU1 配置和模型 staging；必须先有标注权重 |
| 归一化视线 Kalman | 已实现/已测试 | 状态为视线 x/y、角速度、log 尺度/变化率；输出角速度和协方差 |
| KLT/光流 + RANSAC 全局补偿 | 已实现/已测试 | 稀疏角点、patch flow、亚像素 LK、相似变换 RANSAC；平移合成测试 |
| 最多 3 条候选轨迹 | 已实现 | 外观、尺度、速度、全局运动补偿、命中/丢失置信度；主控制仍只输出一条确认轨迹 |
| 五级状态机和 2 帧/35 ms 失效保护 | 已实现/已测试 | 远位置立即重捕获、过期预测继续报告但控制无效 |
| `MotionPrior` 与未来 IMU 接口 | 已实现/已测试 | 四元数、角速度、时间戳、视觉变换和插值；当前允许为空 |
| `TargetEstimate` 与 JSON schema v2 | 已实现/已测试 | 绿灯、装甲、位姿、LOS、状态、安全位、性能字段；保留 v0.1 平铺字段 |
| UART/CAN | 按计划不实现 | 后续直接序列化同一 `TargetEstimate` |
| ROI 导出、完整录像划分、增强、量化选择 | 已实现工具 | `tools/dataset/`；包含 3–20 px 主尺度、全角旋转、方向模糊、遮挡、色温、H.264 往返和结构复制 |
| 0/1 视频硬负样本 | 初始挖掘完成，人工复核待办 | 已从远离确认目标的候选导出 1224 个 ROI 到忽略目录 `datasets/hard_negatives_usb_0_1_v0.2` |
| 九段录像回归 | 已执行 | 最终 480×360 回归位于忽略目录 `recordings/maixcam2/2026-08-30_usb/results_v0.2_fast480_final_2026-09-01`；可提交摘要见性能报告 |
| 单元测试 | 已实现并通过 | 多尺度、捕获锥、任意滚转、PnP、NPU 门控、状态机、JSON、光流、IMU 插值 |
| 板端性能验收 | 无模型 185 分钟通过；温度/NPU 待办 | RSS 无增长且未见持续降频；日志未记录温度，真实模型尚不存在 |
| 300 次 15–25 m 最终试验 | 待采集 | 必须有距离、颜色、滚转、场地和运动真值，不能用九段无标注录像替代 |

## 当前不能伪造为“已完成”的外部产物

1. 原生 480×360 相机内参和畸变参数。
2. 固定镜头下的曝光/增益/WB 扫描结果。
3. 2026 实物灯条中心距、灯条长度和绿灯相对偏移。
4. 至少 1 万正 ROI、2 万硬负 ROI 的五关键点人工标注，以及独立第三场地测试集。
5. 训练后的 `best.pt`、固定输入 ONNX、Pulsar2 生成的两个 AXMODEL 和 MUD。
6. MaixCAM2 上模型 P95 和芯片温度数据（无模型 185 分钟 FPS/延迟/RSS 已通过）。
7. 300 次独立飞行/等效运动试验和角度真值。

这些输入完成前，`npu.enabled` 与 `target_geometry.pose_enabled` 必须保持关闭。启用 NPU 时建议
同时设置 `npu.required=true`，模型加载失败或模型结果与当前绿灯不一致即令
`safe_for_control=false`。

## 验收命令

```bash
cmake -S tests -B build/tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests -j
ctest --test-dir build/tests --output-on-failure

python3 tools/run_v02_regression.py \
  --input-dir recordings/maixcam2/2026-08-30_usb \
  --output-dir recordings/maixcam2/2026-08-30_usb/results_v0.2

python3 tools/evaluate_replay.py RESULT_0.jsonl RESULT_1.jsonl \
  --annotations verified_intervals.json --enforce \
  --output-json reports/generated/replay.json \
  --output-markdown reports/generated/replay.md

python3 tools/evaluate_replay.py RESULT.jsonl \
  --ground-truth-jsonl verified_frame_ground_truth.jsonl --enforce \
  --output-json reports/generated/frame_metrics.json \
  --output-markdown reports/generated/frame_metrics.md

python3 tools/evaluate_device_log.py device_30min.jsonl \
  --output reports/generated/device_30min.json
```

`evaluate_replay.py --enforce` 只接受带人工核验区间或逐帧真值的录像；没有真值会明确失败。
区间真值负责捕获率/控制误报，逐帧真值还负责候选召回、关键点/角度误差和错误换轨。最终门槛：

- 100 ms 捕获成功率 ≥99%；
- 直接观测帧 ≥97%，融合控制有效帧 ≥99.5%；
- 捕获后控制无效间隔 ≤50 ms；
- 无目标区间 `safe_for_control` 次数为 0；
- 绿灯/装甲中心 P95 视线误差 ≤0.1°；
- 位姿重投影 ≤2 px、滚转误差 ≤3°；
- 板端总耗时 P95 ≤30 ms、模型 P95 ≤20 ms、丢帧率 <0.1%。

## 最终构建制品

最终代码已用 MaixCDK 为 `maixcam2` 交叉编译并打包；`file build/dart_green_detect`
确认是 ARM AArch64。安装包已显式携带 `nn` 间接需要的 ALSA 库，`main.sh` 在启动时
补齐其 SONAME 别名。当前无训练模型，因此安装包中的 `models/` 只有占位文件，默认配置
保持 NPU/位姿关闭；完成模型 staging 后必须重新执行 `maixcdk release -p maixcam2`。

```text
b81cfb3247886d94de336b52b6d9c4c86ffbbeca8e99a199ca8b9ef164bee714  build/dart_green_detect
91c647c7b4b7546992183836f29a30d27e92620eb4b7ae5f3dd2393280ef0e32  config/green_detector.conf
21cd97050fd7b34079bf74fbcb8103ef20d92eccc8d4002d9dd972f37f5c86da  dist/dart_green_detect_v0.2.0.zip
```

## v0.1.5 回退基线

本机保留的 v0.1.5 安装包为 `dist/dart_green_detect_v0.1.5.zip`，SHA-256：

```text
20fefaf0ae73ee46171b7a7eb429067b9a5645fe8bca65469c73f2f13215c44a
```

`dist/` 默认不提交 GitHub。发布 v0.2 前应把该包复制到团队制品库并记录板端系统、MaixPy、
MaixCDK commit 与配置版本；不要依赖某一台开发机的忽略目录作为长期回退方案。
