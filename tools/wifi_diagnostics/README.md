# 自研底板 Wi-Fi 诊断工具

本次针对 192.168.137.58 / AIC8800 SDIO 的有界诊断，实测记录位于
`artifacts/wifi_diagnosis_20260915`。这些脚本不安装永久网络恢复服务。

重要发现：当前驱动 a5a6300c… 的 `rwnx_cfg80211_set_power_mgmt` 是直接返回 0
的空实现。`nl_power.py` 的标准 nl80211 读回只是内核状态，不能证明固件真正改变省电。
`power_trial_client.ps1` 初始四阶段的表面开关不能作为有效省电 A/B 结论。

- `power_trial_server.py`：仅接受热点主机 192.168.137.1；端口 5210；随机数据回显；
  最长 600 秒，正常退出时尝试恢复原 nl80211 状态。每 5 秒保存关联、信号和 TCP 计数。
- `power_trial_client.ps1`：Windows 原生 TCP，每阶段 15 秒小包往返、45 秒四连接
  64KiB 往返并校验，超时后重连。吞吐按完整校验成功的回显字节分别计算每个方向，
  包含尾部等待。不是 iperf 极限吞吐或持续单向传输。
- `run_client.py`：为指定加载参数组运行两个相同阶段，保存实际脚本和客户端日志。
- `analyze_power_trial.py FILE`：统计小包 RTT、有效吞吐和客户端异常。
- `station_info.py`：读取固件经驱动上报的 TX_FAILED 等计数，未支持字段不补零。

`ps_module_trial_setup.py` / `he_trial_setup.py` 是本板的临时驱动加载对照：使用
`/run/systemd/system/wifi.service.d/90-ps-diagnostic.conf`，提前设置八分钟回滚 timer。
前者传入 ps_on=0，后者在此基础上增加 he_on=0；需要在板端核对参数和重新关联，
以及热点地址是否变化后再启动客户端。会短暂断开 Wi-Fi，不重启整机。

脚本依赖 `/opt/scripts/wifi.sh` 的准确内容和本板 systemd 服务名，不应直接用于
其他设备。应先保存用户数据、结束采集及当前测试服务。回滚脚本删除仅本次建立的
临时覆盖、重载服务、恢复默认参数并启动 DHCP；确认恢复后取消回滚 timer。

本次已恢复 ps_on=Y、he_on=Y。独立无线发送失败计数不能代替射频层完整统计；
关联快照正常、TX_FAILED=0 都不能单独排除固件或无线问题。
