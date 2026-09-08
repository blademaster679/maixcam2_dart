# OS04A10 实机录像

- [360fps采集，12倍慢放，兼容版12秒](crop360-slow30-compatible.mp4)
- [360fps采集，原始1秒](crop360-realtime.mp4) / [无损12倍慢放12秒](crop360-slow30.mp4)
- [全视野180fps，10.006秒](full180-realtime.mp4) / [6倍慢放60.033秒](full180-slow30.mp4)

360fps片段为连续360帧的一秒NV21内存缓存，随后电脑无损编码；兼容版为有损副本。180fps片段为板端硬件编码，共1,801帧。无插帧，无丢弃帧。场景为当前静态桌面。

180fps全视野NV21通过30分钟测试；360fps采集短测达标，但长测约30秒出现1个缺号，未通过长期连续性验收；360fps实时硬件编码仍未通过。

[完整报告](../../tools/os04a10_highfps/OFFICIAL_VALIDATION.md) · [逐帧记录与源码证据包](verification-evidence.zip)
