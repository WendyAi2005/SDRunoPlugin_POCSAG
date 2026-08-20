# 协议与实现说明

[返回项目首页](../README.md) · [操作指南](USER_GUIDE_CN.md) · [故障排查](TROUBLESHOOTING_CN.md)

## 1. 文档边界

本文描述 v1.0.0 源码当前采用的实现和校验条件，不是中国铁路官方协议规范。铁路字段含义来自现有实测 RAW 数据观察与公开资料对照，仍可能需要更多地区、线路和设备样本验证。

本文不承诺兼容所有 POCSAG 系统、所有铁路频点或所有报文版本。`railway_aux_raw` 等未确认字段只保留原值，不赋予业务含义。

## 2. POCSAG 接收链

当前处理路径可以概括为：

```text
SDRuno NFM audio / samples
  → DC removal and low-pass conditioning
  → zero-crossing / transition timing
  → bit clock and polarity candidates
  → POCSAG sync word
  → 32-bit codeword extraction
  → BCH validation/correction
  → IDLE / ADDRESS / MESSAGE classification
  → RIC + Function + message payload assembly
  → generic mapping or railway application parser
  → RAW log / RailwayTarget state / local map
```

插件支持 512、1200、2400 baud，以及 AUTO、NORMAL、INVERTED 极性。同步后，每个 MESSAGE Codeword 提取 20 位 payload 并按接收顺序跨码字拼接。RAW 同时保留原始码字、纠错后码字、纠错位数与不可纠错状态。

地址后没有任何 Message Codeword 时，消息类型为 `TONE`。有正文时，通用消息默认是 `UNSET`，直到 `(RIC, Function)` 被用户映射为 `NUMERIC` 或 `ALPHA`。

## 3. Railway BASIC：RIC 1234000

### 3.1 当前有效性条件

`IsStrictRailwayBasic()` 仅在下列条件全部满足时将消息标记为有效铁路 BASIC：

- RIC 为 `1234000`。
- 恰好 3 个 Message Codeword，即 60 位、15 个 Numeric 字符。
- 没有不可纠错码字。
- Numeric 正文长度严格为 15。
- 第 6 和第 10 个字符为空格，符合 `xxxxx xxx xxxxx` 布局。
- 三个字段只包含数字、空格或 `-`。

有效性不由 Function 0–3 决定。只要存在正文码字，`1234000` 会先按铁路 Numeric 解释；没有正文码字才是仅呼叫。

### 3.2 Numeric 映射与字段

POCSAG Numeric 字符表为：

```text
084 2.6]195-3U7[
```

15 字符布局：

```text
xxxxx xxx xxxxx
│     │   └─ 公里标，以 0.1 km 为单位
│     └───── 速度，km/h
└─────────── 车次数字部分，允许左侧空格
```

字段完全由 `-` 组成时表示当前没有有效值。例如 `----- --- -----` 仍可通过固定布局检查，但不会生成虚构的车次、速度或公里标；原文继续保留在 RAW。

质量分级中，无纠错的严格有效 BASIC 为 `HIGH`；发生可纠正错误时为 `MEDIUM`；布局无效或存在不可纠错码字为 `LOW`，不会进入正式铁路状态融合。

## 4. Railway EXT：RIC 1234002

### 4.1 当前有效性条件

当前完整 EXT 要求：

- RIC 为 `1234002`。
- 恰好 10 个 Message Codeword，即 200 位。
- 没有不可纠错码字。
- normalized payload 恰好 50 个十六进制 nibble。
- 车次冠字、机车号、端号、线路与坐标通过当前字段校验。

少于 10 个码字标记为 `RAILWAY_EXT_TRUNCATED`。失败原因和 RAW 仍然显示，但无效 EXT 不进入正式状态融合。

### 4.2 每 nibble 位反转

根据现有实测 RAW，当前解析器从 BCH 纠错后的 200 位 message payload 出发，对每个 4-bit nibble 内部独立反转。不是整字节反转，也不是整报文反转。

映射为：

```text
0123456789ABCDEF
↓
084C2A6E195D3B7F
```

结果保存为 `railway_ext_normalized_hex`。该步骤是当前样本驱动的实现选择，不应表述为官方协议定义。

### 4.3 当前字段布局

50 个 normalized hex 字符按当前代码切分为：

| 范围 | 当前解释 |
| --- | --- |
| `[0:4]` | 2 字节 ASCII 车次冠字 |
| `[4:12]` | 8 位十进制机车号 |
| `[12:14]` | 1 字节 ASCII 机车端号 |
| `[14:30]` | 8 字节 CP936/GBK 线路名 |
| `[30:39]` | 经度原始字段 |
| `[39:47]` | 纬度原始字段 |
| `[47:50]` | `railway_aux_raw`，未知/保留原始字段 |

机车号在内部还拆分保存当前类型代码和序列部分；对用户显示仍以完整 8 位机车号为主。`railway_aux_raw` 不解释为状态、校验、方向或 GPS 标志。

## 5. 经纬度

当前坐标解析保留三层：

1. 原始字段，如 `112546296` / `27527550`。
2. NMEA-style 度分，如 `112°54.6296′ E` / `27°52.7550′ N`。
3. 十进制度，供地图和距离计算使用。

当前规则：

- 经度：`dddmm.mmmm`，前三位为度。
- 纬度：`ddmm.mmmm`，前两位为度。
- 分值必须小于 60。
- 经度范围不超过 180°，纬度范围不超过 90°。

例如：

```text
112546296 → 112°54.6296′ E → 112 + 54.6296 / 60
           = 112.910493333...

27527550  → 27°52.7550′ N  → 27 + 52.7550 / 60
           = 27.879250000...
```

无效字段不会强制画到地图。浏览器的坐标格式、datum 与显示转换只作用于 marker 计算，不回写底层 RAW。

## 6. BASIC / EXT 组装

`RailwayMessageAssembler` 先收集一个 transmission 内所有严格有效的 BASIC 与 EXT，并按出现顺序成对组装：

- 同 transmission 成对：`FULL`，`pairing_method=TRANSMISSION_ID`。
- 剩余 BASIC：`BASIC ONLY`，`pairing_method=INDEPENDENT`。
- 剩余 EXT：`EXT ONLY`，`pairing_method=INDEPENDENT`。

因此部分记录是正常状态，而不是 UI 占位错误。

## 7. RailwayStateManager

每个真实目标使用进程内自增的永久 `target_uid`。车次号、机车号、机车端号和 transmission 都是可补全的属性与索引，不是容器主键。

状态管理器维护：

- 车次别名 → `target_uid`
- 机车号 + 端号 → `target_uid`
- 机车号 → `target_uid`
- transmission → `target_uid`

同一索引直接命中时复用已有目标。对于互补的 `BASIC ONLY` 与 `EXT ONLY`，候选评分考虑 transmission、身份、时间差、线路、GPS 距离和公里标连续性；阈值为 60。同车次或同机车身份冲突时执行 `MergeTargets()`。

合并会保留主 UID，更新字段和别名，合并、排序、去重轨迹，并删除次目标。MapServer 对快照差异发送 `target_remove` 和 `target_update`，浏览器以 `target_uid` 为 marker 字典键，避免残留以车次或机车文本命名的重复 marker。

目标 120 秒未更新标记为 `STALE`，300 秒后删除；每个目标最多保留 100 个轨迹点。`created_by`、`merge_count`、`last_merge_reason` 等字段用于诊断身份融合。

## 8. MileagePositionDatabase

本地数据库的键是“原始线路名 + 0.1 km 公里标”。每个锚点保存均值坐标、样本数、离群数、位置离散度、首末时间、来源与质量。

### 8.1 学习条件

仅当目标同时满足以下约束时学习：

- 有效 BASIC 和有效 EXT。
- 有公里标、线路和无线 GPS。
- BASIC/EXT 更新时间相差不超过 5 秒。
- BASIC 为严格 3 CW、EXT 为严格 10 CW。
- 两者均无不可纠错码字且质量为 HIGH。
- 通过当前运动连续性检查。

新样本距已有锚点均值超过 500 m 时作为离群值拒绝。

### 8.2 查询

查询优先级：

1. 本地同线路、同公里标精确锚点。
2. 同线路前后锚点之间的线性插值，锚点间隔必须不超过 2.0 km。
3. OSM 异步查询与缓存。

只有单侧锚点时不外推。位置来源分别记录为 `RADIO_GPS`、`LOCAL_MILEAGE_EXACT`、`LOCAL_MILEAGE_INTERPOLATED`、`OSM_MILEAGE_EXACT`、`OSM_MILEAGE_INTERPOLATED` 或 `NO_POSITION`。

OSM 查询要求线路标签与公里标共同唯一匹配，5 秒超时；若有多个候选则返回歧义，不强行选择。公开 OSM 铁路里程数据并不保证完整。

## 9. 本机服务接口

MapServer 仅绑定 `127.0.0.1:8765`。当前路由包括：

- `GET /api/trains`
- `GET /ws`
- `GET /api/mileage/anchors`
- `GET /api/mileage/export`
- `GET /api/mileage/lookup?line=...&km=...`
- `POST /api/mileage/import`
- `POST /api/mileage/clear`
- `POST /api/mileage/learning`
- `GET /`、`/index.html`、`/app.js`、`/style.css`

当前 WebSocket 推送 `snapshot`、`target_update` 与 `target_remove`。这是本机诊断接口，不应暴露为公网服务。

## 10. 原始数据保留原则

CSV/JSONL 保留 message bits、原始/纠错码字、`message_hex`、Numeric/Alpha 结果、`railway_ext_normalized_hex`、坐标 RAW、度分、十进制度和 `railway_aux_raw`。解析失败时优先保留证据，不把不确定内容包装为正式目标。

## 11. 已知研究边界

- Railway BASIC/EXT 字段语义是实验性解释，不是官方规范声明。
- EXT 结构可能存在尚未观察到的版本或设备差异。
- `railway_aux_raw` 含义未知。
- 网页默认线路“沪昆线”是显示/查询缺省值，不代表无线报文来源。
- 坐标源 datum 当前不能仅凭字段格式完全确认，网页因此保留 WGS84 / GCJ-02 / BD-09 对照。
- OSM 数据、瓦片服务与线路公里标标注可能缺失、延迟或存在歧义。
