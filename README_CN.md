# SDRuno POCSAG 列车接近预警接收插件

这是一个只接收的 SDRuno 社区插件。它直接读取 SDRuno 内部的 NFM 音频流并解码 POCSAG，不需要 VB-CABLE、PDW，也不包含任何发射功能。

> 当前为实验测试版。解码器已通过模拟 POCSAG 1200 数字消息和 `TONE ONLY` 呼叫测试，并参考 PDW 的音频零交叉锁相思路增强了现场信号同步；不同接收机、音频链和信号质量仍可能需要继续调试。

## 默认用途

- 接收频率：821.237500 MHz
- 调制：NFM
- 滤波带宽：15 kHz
- 默认速率：POCSAG 1200 baud
- 铁路预设极性：INVERTED（其他频率仍可选 AUTO / NORMAL / INVERTED）
- 支持速率：512 / 1200 / 2400 baud
- 输出：合并后的列车预警、通用 POCSAG 原始消息、RAW 与 BCH 状态

## 1234000 铁路正文

- `1234000` 只要存在 Message Codeword，Function 0～3 都优先按标准 POCSAG Numeric 表解释。
- 15 字符格式 `xxxxx xxx xxxxx` 映射为车次、速度 km/h、公里标（0.1 km）。
- `-` 是合法的“未知字段”占位，例如 `86813 --- -----`。
- 只有没有任何 Message Codeword 时才显示 `TONE / 仅呼叫`。
- JSONL/CSV 继续保留原始码字和 message bits，并增加铁路字段与可信度。

## 1234002 铁路扩展正文

- 完整报文按 `10 Message CW × 20 bit = 200 bit` 处理，类型为 `RAILWAY_EXT`。
- 从 BCH 纠错后的 message bits 出发，对每个 4-bit nibble 独立反转；不做整字节或整报文反转。
- 50 个 normalized hex nibble 依次解析为：车次冠字、8 位本务机车号、机车端号、GBK 线路名、经度、纬度和未解释的 `railway_aux_raw`。
- 经纬度原始字段按去掉小数点的 NMEA 度分格式解释：经度 `dddmm.mmmm`，纬度 `ddmm.mmmm`；仅在分值小于 60 且范围合法时转换为十进制度。
- 状态、CSV 和 JSONL 同时保留原始字段、可读度分字符串与转换后的十进制度；`railway_normalized_hex` 和所有既有 RAW 字段不变。
- 少于 10 CW 显示 `RAILWAY_EXT_TRUNCATED`；字段非法或存在不可纠正码字时显示 RAW，不进入正式合并结果。
- `railway_aux_raw` 只原样保留，不解释为状态、校验或 GPS 标志。

## 当前列车状态

- 每个真实目标使用进程内永久自增 `target_uid`；车次、机车号、端号和 transmission 都是可补全的属性或索引，不再作为容器主键。
- 状态管理器维护车次、机车号+端号、机车号和 transmission 索引；同机车端号变化默认更新同一 UID。
- FULL 更新同时命中旧 BASIC_ONLY 与 EXT_ONLY 对象时执行 `MergeTargets()`：合并字段、轨迹、别名与调试信息并删除次目标。
- 开发调试字段包括 `created_by`、`merge_count`、`last_merge_reason`；详情窗口显示稳定 UID。
- 每次实时 POCSAG transmission 都分配 `transmission_id`。
- 合法 `1234000`（严格 3 CW、无不可纠错、15 字符固定格式）可以独立生成 `BASIC ONLY` 目标；其他看似数字的损坏报文只进入 RAW。
- 合法 `1234002`（严格 10 CW、无不可纠错、NMEA 度分字段合法且分值小于 60）可以独立生成 `EXT ONLY` 目标并进入地图。
- 同一 transmission 优先按 `TRANSMISSION_ID` 合并；跨突发可在 2 秒窗口内按 `TIME_FALLBACK` 补全，后续更新通过 `TARGET_STATE` 复用车次或机车目标。
- `----- --- -----` 是合法未知占位，不会丢掉同组 EXT 的机车与 GPS。
- 默认页“列车接近预警”显示当前目标状态而不是报文历史；同一列车更新原行。120 秒无更新标记 `STALE`，300 秒后移除。
- 每个目标最多保留最近 100 个 WGS84 轨迹点。
- “POCSAG 原始消息”页继续保留所有通用消息、完整 RAW 字段和手动正文映射。
- 选择预警记录后点击“协议 / RAW 详情”（或双击）可查看两个 RIC 的 message bits、message hex、原始/纠错后 Codeword 和扩展 normalized hex。

## 浏览器实时地图

- 点击插件里的“打开实时地图”，会启动同目录下的 `RailwayMapServer.exe` 并打开 <http://127.0.0.1:8765/>。
- 服务器只绑定 `127.0.0.1`，提供 `/api/trains` 和 `/ws`；不会监听局域网地址。
- 浏览器使用 Leaflet + OpenStreetMap 显示目标与最近 100 个轨迹点；同一目标只更新一个 marker。
- marker 和轨迹严格以 `target_uid` 为键；WebSocket 同时提供 `target_update` 与 `target_remove`，归并后会明确删除次目标 marker。
- 坐标调试区支持原始格式 `AUTO / NMEA 度分 / 十进制度`、源坐标系 `WGS84 / GCJ-02 / BD-09`，以及六种不转换或坐标系转换模式。
- 默认使用 `NMEA 度分 + WGS84 + 不转换`；“恢复默认”只恢复浏览器显示选项，不改写底层 RAW 数据。
- marker 弹窗同时显示原始字段、度分格式、十进制度、地图实际坐标及当前转换模式。格式或范围非法的 RAW 坐标不会绘制到地图。
- 无法访问互联网或加载 OSM/Leaflet 时，当前列车列表、坐标、速度、车次和线路仍可查看，地图区显示“底图不可用”。
- 插件与地图服务通过 `%LOCALAPPDATA%\SDRunoPlugin_POCSAG\railway_state.json` 解耦，地图服务异常不会拖死 SDRuno。

## 线路公里标定位

- 插件将原始无线坐标保存在 `radio_longitude/radio_latitude`，地图使用的位置单独保存在 `display_longitude/display_latitude`，估算位置绝不会写回或伪装成无线 GPS。
- `%LOCALAPPDATA%\SDRunoPlugin_POCSAG\mileage_positions.json` 按“原始线路名 + 0.1 km 公里标”持久化锚点、均值坐标、样本数、离散度、离群数、首末时间、来源和质量。
- 只有严格 3 CW 的高质量 BASIC 与严格 10 CW 的高质量 EXT 在 5 秒内正确配对且运动连续时，才学习公里标与 GPS；500 m 以上新样本拒绝进入均值。
- 无新鲜无线 GPS 时，先查完全相同公里标；否则仅在同线路前后锚点间隔不超过 2.0 km 时做经纬度线性插值；只有单侧锚点时不外推。
- `RailwayTarget` 和轨迹点携带 `PositionSource`、质量和置信度；位置优先级为 RADIO GPS、LOCAL exact、LOCAL interpolated、OSM exact、OSM interpolated、NO_POSITION。
- 浏览器支持 AUTO / RADIO GPS ONLY / MILEAGE ONLY / OSM ONLY，支持 GPS 与公里标双点偏差线、学习锚点图层、线路学习摘要、手工线路+公里标查询、学习开关和数据库导入/导出/清空。
- MapServer 新增 `/api/mileage/anchors`、`/api/mileage/lookup`、`/api/mileage/export`、`/api/mileage/import`、`/api/mileage/clear` 和 `/api/mileage/learning`。
- OSM 查询只在本地数据库和 `%LOCALAPPDATA%\SDRunoPlugin_POCSAG\osm_mileage_cache.json` 都未命中时异步执行，5 秒超时；必须由线路标签和公里标共同唯一匹配，否则返回 `AMBIGUOUS_OSM_MILEAGE` 或无结果。

## 正文解码映射

POCSAG 不会在正文里统一标明它应按 Numeric 还是 Alpha 解释。插件因此不再根据字符外观自动猜测：

- 地址后没有正文码字：自动显示 `TONE / 仅呼叫 / 无文本`。
- 有正文但 `(RIC, Function)` 尚未配置：显示 `UNSET / 未设置`，并保留 `RAW` 十六进制数据。
- 点击“正文映射”，可为指定 `(RIC, Function)` 添加、更新或删除 `NUMERIC / ALPHA` 解释规则。
- 也可右键一条带 RAW 的消息，直接设置为 `NUMERIC` 或 `ALPHA`，该行会立即重新解释。

映射会持久化到：

`%APPDATA%\SDRunoPlugin_POCSAG\message_mappings.tsv`

## 安装

1. 关闭 SDRuno。
2. 将 `release\SDRunoPlugin_POCSAG.dll`、`RailwayMapServer.exe` 和 `web` 文件夹复制到：
   `C:\Users\你的用户名\Documents\CommunityPlugins\`
3. 重新启动 SDRuno，在插件面板加载 `POCSAG Railway Alert`。
4. 点击插件窗口中的“一键设置 821.2375 MHz / NFM / 15 kHz”。

插件仅用于无线电技术实验。不能把普通 SDR 和本插件作为铁路作业或人身安全防护设备，也不要公开传播接收到的非公开铁路业务信息。

## 编译

- Visual Studio 2022 Build Tools
- Desktop development with C++（MSVC v143 + Windows 10/11 SDK）
- 官方 SDRplay 插件 SDK：<https://github.com/SDRplay/plugins>
- 构建目标：`Release | x86`

将官方 SDK 克隆到本仓库的同级目录，并命名为 `SDRplay-plugins-sdk`：

```powershell
git clone https://github.com/SDRplay/plugins ../SDRplay-plugins-sdk
```

```powershell
msbuild SDRunoPlugin_POCSAG.sln /p:Configuration=Release /p:Platform=x86
```

生成文件位于 `build\SDRunoPlugin_POCSAG.dll`、`build\RailwayMapServer.exe` 和 `build\web`。

## 开源参考

- SDRuno 插件接口：SDRplay 官方 Plugins SDK
- 音频同步思路参考：开源 [PDW Paging Decoder](https://github.com/Discriminator/PDW)
