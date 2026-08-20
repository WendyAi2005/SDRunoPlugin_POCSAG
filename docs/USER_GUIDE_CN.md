# SDRuno POCSAG Railway Alert 中文操作指南

[返回项目首页](../README.md) · [协议说明](PROTOCOL_NOTES_CN.md) · [故障排查](TROUBLESHOOTING_CN.md)

## 1. 使用范围

本插件是 SDRuno Community Plugin，用于接收和分析 POCSAG，并对当前实测铁路消息做实验性应用层解析。它仅接收，不发射，也不控制铁路设备。

本指南面向第一次安装和使用发布包的用户。协议边界、字段结构和状态融合算法见[协议与实现说明](PROTOCOL_NOTES_CN.md)。

## 2. 软件组成

`release/` 的可运行部分由以下文件组成：

| 文件 | 用途 |
| --- | --- |
| `SDRunoPlugin_POCSAG.dll` | SDRuno x86 Community Plugin |
| `RailwayMapServer.exe` | 本机 HTTP/WebSocket 地图服务 |
| `web/index.html` | 地图页面 |
| `web/app.js` | 目标、轨迹、坐标与公里标交互逻辑 |
| `web/style.css` | 地图页面样式 |
| `README_CN.md` | 发布包内的简要安装说明 |

`RailwayMapServer.exe` 会从自身所在目录读取 `web/`。不要把 EXE 与 `web/` 拆散。

## 3. 安装

### 3.1 前提

- Windows 与 32 位插件环境。
- 支持 Community Plugin 的 SDRuno；v1.0.0 发布包已在 SDRuno 1.41.1 环境验证。
- Microsoft Visual C++ 2015–2022 Redistributable（x86）。如果系统已具备 `MSVCP140.dll` 和 `VCRUNTIME140.dll`，无需重复安装。

### 3.2 复制文件

1. 完全退出 SDRuno，确认进程不再占用旧 DLL。
2. 推荐创建：

   ```text
   %USERPROFILE%\Documents\CommunityPlugins\
   ```

3. 将 DLL、EXE 和整个 `web/` 文件夹复制到该目录：

   ```text
   CommunityPlugins/
   ├─ SDRunoPlugin_POCSAG.dll
   ├─ RailwayMapServer.exe
   └─ web/
      ├─ index.html
      ├─ app.js
      └─ style.css
   ```

4. 启动 SDRuno，在 `PLUGINS` / `Plugin Control` 中选择对应 Community Plugin 目录并加载插件。

Community Plugin 目录可按 SDRuno 的配置选择，上述 Documents 路径是推荐布局，不是代码硬编码的唯一位置。

## 4. 第一次运行

1. 连接并选择接收设备。
2. 启动 SDRuno stream。
3. 打开本插件。
4. 若要测试当前铁路实测频点，点击 `一键设置 821.2375 MHz / NFM / 15 kHz`。
5. 确认速率为 `1200`、极性为 `INVERTED`。
6. 观察状态栏的输入百分比、跳变、同步、有效、错误和 RAW 段计数。

上述预设只针对当前实验场景。接收其他 POCSAG 信号时，应使用该信号实际频率、带宽、速率和极性。

## 5. 顶部控件与按钮

| 控件/按钮 | 作用 |
| --- | --- |
| `一键设置 821.2375 MHz / NFM / 15 kHz` | 设置当前铁路实验预设，并选择 1200 baud、INVERTED |
| `速率` | 选择 512、1200 或 2400 baud |
| `极性` | 选择 AUTO、NORMAL 或 INVERTED |
| `收到消息蜂鸣` | 每次完成消息时播放提示音 |
| `清空记录` | 清空当前插件界面的消息与目标状态；不等同于清空公里标学习数据库 |
| `正文映射` | 管理通用 POCSAG 的 `(RIC, Function)` → NUMERIC/ALPHA 规则 |
| `复制最近 RAW` | 将最近完成的 transmission RAW 复制到剪贴板 |
| `导出 RAW 日志` | 同时导出 JSONL 和 CSV |
| `协议 / RAW 详情` | 查看选中铁路目标对应的 BASIC/EXT 位流、码字和 normalized hex |
| `打开实时地图` | 启动本机 MapServer 并打开浏览器地图 |

插件状态栏中的计数用于诊断接收链。同步计数增加但有效消息很少，通常提示频偏、噪声、过载、速率或极性仍需调整。

## 6. 两个页面的区别

### 列车接近预警

这是“当前 RailwayTarget 状态”页面，不是追加式报文日志。同一个 `target_uid` 后续收到 BASIC 或 EXT 时，程序更新原行；合并目标时只保留主目标。

常见字段包括车次、速度、公里标、机车号、端号、线路、经纬度、数据完整度和质量。字段是否出现取决于实际收到的报文。

### POCSAG 原始消息

这是按接收顺序追加的通用消息历史。它显示 RIC、Function、解释类型、正文和 RAW，适合判断消息是仅呼叫、未设置、NUMERIC、ALPHA 或铁路专用解析。

## 7. 目标状态

| 状态 | 含义 |
| --- | --- |
| `FULL` | 当前目标已经具备 BASIC 和 EXT |
| `BASIC ONLY` | 只有有效 BASIC；可能有车次、速度或公里标，没有机车/GPS |
| `EXT ONLY` | 只有有效 EXT；可能有机车、线路或 GPS，没有 BASIC |
| `STALE` | 120 秒没有新更新，目标仍保留显示 |

目标超过 300 秒没有更新后会从当前状态中移除。BASIC/EXT 可以异步或独立到达，因此 `BASIC ONLY` 与 `EXT ONLY` 本身不代表解码失败。

`----- --- -----` 是当前 BASIC 数字布局允许的未知占位：车次、速度和公里标不会被伪造为有效数值；原始内容仍保留在 RAW 中。如果同组有有效 EXT，机车与 GPS 仍可形成目标。

## 8. 正文映射

POCSAG 的 Function 不能通用于判断正文一定是 Numeric 还是 Alpha。本插件使用 `(RIC, Function)` 规则表：

- 地址后没有 Message Codeword：`TONE / 仅呼叫 / 无文本`。
- 有正文但没有规则：`UNSET / 未设置`，同时显示 RAW。
- 规则为 `NUMERIC`：使用标准 POCSAG 数字字符表解释。
- 规则为 `ALPHA`：使用 7-bit 字母数字方式解释。

点击 `正文映射` 可添加、更新或删除规则；也可在原始消息上右键直接设置。规则保存在：

```text
%APPDATA%\SDRunoPlugin_POCSAG\message_mappings.tsv
```

铁路专用 RIC 的解析优先于这张通用映射表。

## 9. RAW 日志

遇到乱码、纠错异常、目标未合并、坐标异常或新 RIC 时，RAW 比截图更适合复现问题。点击 `导出 RAW 日志` 并选择一个文件名；插件会生成同名 `.jsonl` 与 `.csv`。

RAW 中包含频率、速率、极性、transmission、原始/纠错码字、消息位流、Numeric/Alpha 结果、铁路字段和质量信息。分享前请检查内容并遵守当地法律与信息传播要求。

## 10. 实时地图

点击 `打开实时地图`，或在 MapServer 已运行时访问：

<http://127.0.0.1:8765/>

页面左侧显示当前目标，地图显示具有可靠位置的 marker、详情弹窗和最多 100 个最近轨迹点。目标和 marker 均以稳定 `target_uid` 更新；没有合法位置的目标只显示在列表中，不会被放到 `(0,0)`。

地图服务仅监听本机。它读取：

```text
%LOCALAPPDATA%\SDRunoPlugin_POCSAG\railway_state.json
```

Leaflet 脚本与 OpenStreetMap 瓦片从互联网加载。底图加载失败不会停止 SDR 解码，也不代表本机目标接口失效。

## 11. 坐标调试与显示

浏览器提供三组设置：

1. 原始格式：`AUTO`、`NMEA 度分`、`十进制度`。
2. 源坐标系：`WGS84`、`GCJ-02`、`BD-09`。
3. 地图显示转换：不转换，以及 WGS84 / GCJ-02 / BD-09 之间代码已提供的转换组合。

默认是 `NMEA 度分 + WGS84 + 不转换`。点击 `恢复默认` 可恢复浏览器显示设置。切换只改变 marker 的显示计算，不修改插件保存的 RAW、度分或十进制度字段。

弹窗会同时显示 RAW、度分、无线 GPS、当前显示坐标、实际地图坐标、位置来源、质量和转换模式。若分值大于等于 60、范围不合法或字段缺失，则坐标为 invalid，不绘制 marker。

## 12. 线路与公里标定位

网页默认线路为“沪昆线”。无线 EXT 给出有效线路时优先使用无线线路；线路缺失时使用网页默认值。每个目标卡片还可手动 `修改` 线路，或点击 `自动` 恢复无线/默认线路。网页覆盖保存在浏览器 localStorage，不修改底层 RAW。

位置优先级支持：

- `AUTO`
- `RADIO GPS ONLY`
- `MILEAGE ONLY`
- `OSM ONLY`

本地数据库只从满足质量、时间配对和运动连续性约束的线路 + 公里标 + GPS 样本学习锚点。查询时先尝试完全相同公里标；若同线路前后锚点间隔不超过 2.0 km，才做线性插值，不做单侧外推。

网页可以显示 GPS 与公里标对比、显示学习锚点、开关自动学习、手工输入线路与公里标定位，并导入、导出或清空数据库。清空学习数据不可撤销，操作前应先导出备份。

数据文件位于：

```text
%LOCALAPPDATA%\SDRunoPlugin_POCSAG\mileage_positions.json
%LOCALAPPDATA%\SDRunoPlugin_POCSAG\osm_mileage_cache.json
```

## 13. 安全与法律

本项目是无线电、SDR 与数字通信协议研究工具，仅接收，不具备铁路安全认证。不得把显示结果用于铁路调度、列车运行控制、人身安全预警或铁路作业防护。无线电接收、记录和传播应遵守使用者所在地法律法规。

## 14. 遇到问题

先查看[常见问题与故障排查](TROUBLESHOOTING_CN.md)。若仍无法定位，导出 RAW，并记录发生时间、频率、baud、极性、SDRuno 版本、插件截图与问题描述。
