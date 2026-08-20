# SDRuno POCSAG Railway Alert

面向 SDRuno 的 POCSAG 接收、铁路列车接近预警解析与实时地图实验插件。

> [!IMPORTANT]
> 本项目仅接收（RX-only / receive-only），不实现无线电发射，也不控制任何铁路设备。它用于 SDR、无线电数字通信和协议研究，不得作为铁路调度、列车运行控制、人身安全预警或铁路作业防护的正式依据。

## 项目简介

这是一个 Windows x86 SDRuno Community Plugin。插件直接读取 SDRuno 提供的 NFM 音频流并完成 POCSAG 解码，不需要 VB-CABLE、Virtual Audio Cable 或 PDW 音频环回。

在通用 POCSAG 接收之外，项目还包含一套基于实测报文和公开资料实现的实验性铁路应用层解析器。它把 Railway BASIC 与 Railway EXT 中可用的车次、速度、公里标、机车号、线路和位置等字段，融合为持续更新的当前目标，并可在本机浏览器地图中查看。

本项目不是 SDRplay 官方产品，也不声称是官方铁路协议实现、全国铁路通用解码器或铁路安全系统。

## 主要功能

- SDRuno 插件内直接接收，无需虚拟声卡或外部音频回环。
- 支持 POCSAG 512 / 1200 / 2400 baud。
- 支持 `AUTO`、`NORMAL`、`INVERTED` 极性。
- 提供同步、BCH 纠错、RIC、Function、Message Codeword 和 RAW 诊断信息。
- 支持按 `(RIC, Function)` 手动配置 `NUMERIC` / `ALPHA` 正文解释；无正文时识别为 `TONE`。
- 可导出 JSONL 与 CSV RAW 日志，保留原始码字、纠错后码字和消息位流。
- 实验性解析 `RIC=1234000` Railway BASIC 和 `RIC=1234002` Railway EXT。
- BASIC 与 EXT 可独立显示为 `BASIC ONLY` / `EXT ONLY`，后续由 `RailwayStateManager` 尝试补全为同一目标。
- 使用稳定 `target_uid` 管理当前目标，融合车次、机车、线路、GPS 与轨迹。
- 本机 HTTP/WebSocket 实时地图，显示目标、详情和最近轨迹。
- 保留坐标 RAW，并支持 NMEA 度分、十进制度及 WGS84 / GCJ-02 / BD-09 显示对照。
- 支持线路 + 公里标的本地锚点学习、精确匹配、有限区间插值及实验性 OSM 查询/缓存。

## 铁路消息与部分记录

当前解析器针对已经观察到的两类应用层消息：

- Railway BASIC：车次、速度、公里标。
- Railway EXT：车次冠字、机车号、机车端号、线路、坐标和一个未解释的保留字段。

两部分不保证同时到达。例如只收到 `K358 / 83 km/h / K1126.1` 时，目标仍可作为 `BASIC ONLY` 显示；只收到机车号、线路与 GPS 时，也可作为 `EXT ONLY` 显示。目标管理器会按 transmission、已知身份与受限时间窗口尝试合并，不能匹配时则保留独立目标，避免强行拼接不同列车。

字段含义来自当前实测 RAW 数据的实验性分析，可能随线路、设备或业务系统而不同。详细边界见[协议与实现说明](docs/PROTOCOL_NOTES_CN.md)。

## 快速开始

### 运行环境

- Windows。
- 支持 Community Plugin 的 SDRuno；当前发布包面向已验证的 SDRuno 1.41.1 32 位插件环境。
- 可用的 SDRplay / SDRuno 接收链。
- 如系统缺少运行库，请安装 Microsoft Visual C++ 2015–2022 Redistributable（x86）。

### 安装

1. 完全退出 SDRuno。
2. 从 `release/` 取得以下内容，并保持相对目录不变：

   ```text
   SDRunoPlugin_POCSAG.dll
   RailwayMapServer.exe
   web/
     index.html
     app.js
     style.css
   ```

3. 推荐创建 `%USERPROFILE%\Documents\CommunityPlugins\`，把上述 DLL、EXE 和 `web/` 一起复制进去。也可使用 SDRuno Plugin Control 已配置的其他 Community Plugin 目录。
4. 启动 SDRuno，在 `PLUGINS` / `Plugin Control` 中选择对应 Community Plugin 目录并加载本插件。

SDRplay 的官方 Plugins SDK 当前说明插件接口版本 2 需要 SDRuno 1.40.2 或更高版本；本项目发布包仍以实际验证过的 1.41.1 x86 环境为准。

### 铁路实验预设

启动接收流后，点击插件顶部：

`一键设置 821.2375 MHz / NFM / 15 kHz`

当前铁路实验预设为：

- 频率：821.2375 MHz
- 模式：NFM
- 带宽：15 kHz
- 速率：1200 baud
- 极性：INVERTED

这是当前实测频点的便捷预设，不代表中国所有铁路或所有地区使用相同参数。其他信号可手动选择速率和极性。

## 本机实时地图

点击插件中的 `打开实时地图`。插件会启动与 DLL 同目录的 `RailwayMapServer.exe`，并打开：

<http://127.0.0.1:8765/>

地图服务只绑定 `127.0.0.1`，通过 HTTP 提供当前快照并通过 WebSocket 推送目标更新。浏览器端使用 Leaflet 1.9.4 和 OpenStreetMap 在线底图；无互联网时，目标列表和本机数据接口仍可工作，但底图可能不可用。

地图以 `target_uid` 更新 marker 和轨迹。坐标切换只重新计算浏览器显示位置，不覆盖插件保存的 RAW 坐标。默认使用 `NMEA 度分 + WGS84 + 不转换`；线路缺失时网页默认显示“沪昆线”，也可按目标手动修改。

公里标定位可以从高质量的线路、公里标与无线 GPS 配对中学习本地锚点。没有新鲜 GPS 时，仅在满足数据库约束时使用精确锚点或相邻锚点插值；OSM 查询结果也可能因公开数据不完整或歧义而不可用。

## 数据与隐私

运行时数据默认保存在：

- `%APPDATA%\SDRunoPlugin_POCSAG\message_mappings.tsv`：正文映射。
- `%LOCALAPPDATA%\SDRunoPlugin_POCSAG\railway_state.json`：当前目标快照。
- `%LOCALAPPDATA%\SDRunoPlugin_POCSAG\mileage_positions.json`：本地公里标锚点。
- `%LOCALAPPDATA%\SDRunoPlugin_POCSAG\osm_mileage_cache.json`：OSM 查询缓存。

RAW 日志仅在用户点击 `导出 RAW 日志` 后写入所选位置。日志可能包含接收时间、频率、RIC、原始码字和解析字段；分享前请检查并遵守当地法律、频谱使用规则与信息传播要求。

## 文档

- [中文完整操作指南](docs/USER_GUIDE_CN.md)
- [协议与实现说明](docs/PROTOCOL_NOTES_CN.md)
- [常见问题与故障排查](docs/TROUBLESHOOTING_CN.md)
- [v1.0.0 Release 说明](docs/RELEASE_NOTES_v1.0.0.md)
- [开发与技术资料](README_CN.md)

<!-- TODO: add verified plugin-window and real-time-map screenshots. -->

## 从源码构建

构建需要 Visual Studio 2022 Build Tools、MSVC v143、Windows 10/11 SDK，以及 [SDRplay SDRuno Plugins SDK](https://github.com/SDRplay/plugins)。将 SDK 克隆为本仓库同级目录 `SDRplay-plugins-sdk`，然后构建 `Release | x86`：

```powershell
git clone https://github.com/SDRplay/plugins ../SDRplay-plugins-sdk
msbuild SDRunoPlugin_POCSAG.sln /p:Configuration=Release /p:Platform=x86
```

生成物位于 `build/`。开发细节和源文件说明见[开发与技术资料](README_CN.md)。

## Acknowledgements / 致谢

- [SDRplay SDRuno Plugins SDK](https://github.com/SDRplay/plugins)：插件接口与开发模板。
- [Leaflet](https://leafletjs.com/)：浏览器交互式地图。
- [OpenStreetMap](https://www.openstreetmap.org/copyright)：地图数据与在线瓦片，版权归 OpenStreetMap contributors。
- 公开 POCSAG 技术资料，以及 PDW 项目中基于音频过零间隔进行同步的思路参考。当前仓库未声明移植 PDW 源码。

SDRuno 和 SDRplay 是其各自权利人的产品或商标。本项目与 SDRplay 无官方隶属关系。

## 安全与法律说明

本项目按“现状”提供，是仅接收的实验研究工具，不具备铁路安全认证。不得将其输出作为铁路调度、列车运行控制、人身安全预警或铁路作业防护的依据。无线电接收、记录、分析和传播应遵守使用者所在地法律法规及相关授权要求。

## License

本项目以 [MIT License](LICENSE) 发布。第三方组件与数据仍适用各自许可证和使用条款。
