# SDRuno POCSAG Railway Alert v1.0.0

## 首个公开测试版本

v1.0.0 是面向 SDRuno 1.41.1 x86 插件环境整理的首个公开测试版本。它是仅接收（RX-only）的 SDR 与数字通信协议研究工具，不实现无线电发射，也不控制铁路设备。

## 主要功能

- SDRuno 内部 NFM 音频流 POCSAG 接收，无需虚拟声卡或 PDW 音频回环。
- 512 / 1200 / 2400 baud，AUTO / NORMAL / INVERTED 极性。
- POCSAG 同步、BCH 纠错、RIC / Function / Message Codeword 与 RAW 诊断。
- `(RIC, Function)` Numeric / Alpha 正文映射。
- 基于实测数据的实验性 Railway BASIC (`1234000`) / EXT (`1234002`) 解析。
- `RailwayTarget` 当前状态融合，支持 FULL / BASIC ONLY / EXT ONLY / STALE。
- 本机 Leaflet + OpenStreetMap 实时地图和 WebSocket 更新。
- NMEA-style 度分、十进制度及 WGS84 / GCJ-02 / BD-09 显示对照。
- 线路 + 公里标本地锚点学习、有限插值和实验性 OSM 查询/缓存。
- JSONL / CSV RAW 导出。

## 包含文件

```text
SDRunoPlugin_POCSAG.dll
RailwayMapServer.exe
README_CN.md
web/
  index.html
  app.js
  style.css
```

## 安装

1. 完全退出 SDRuno。
2. 将 DLL、EXE 和完整 `web/` 复制到 SDRuno Plugin Control 使用的 Community Plugin 目录；推荐 `%USERPROFILE%\Documents\CommunityPlugins\`。
3. 启动 SDRuno，在 Plugin Control 中加载插件。
4. 点击 `一键设置 821.2375 MHz / NFM / 15 kHz` 使用当前铁路实验预设，或手动设置其他信号参数。
5. 点击 `打开实时地图`，访问 <http://127.0.0.1:8765/>。

如系统提示缺少 `MSVCP140.dll` 或 `VCRUNTIME140.dll`，请安装 Microsoft Visual C++ 2015–2022 Redistributable（x86）。

完整说明：[中文操作指南](https://github.com/WendyAi2005/SDRunoPlugin_POCSAG/blob/release/v1.0.0/docs/USER_GUIDE_CN.md) · [协议说明](https://github.com/WendyAi2005/SDRunoPlugin_POCSAG/blob/release/v1.0.0/docs/PROTOCOL_NOTES_CN.md) · [故障排查](https://github.com/WendyAi2005/SDRunoPlugin_POCSAG/blob/release/v1.0.0/docs/TROUBLESHOOTING_CN.md)

## 注意

这是实验性质的 RX-only 工具，不是官方铁路协议实现或铁路安全系统。不得将其用于铁路调度、列车运行控制、人身安全预警或铁路作业防护。无线电接收、记录和传播应遵守当地法律法规。
