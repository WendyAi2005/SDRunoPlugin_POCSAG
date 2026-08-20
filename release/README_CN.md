# SDRuno POCSAG Railway Alert v1.0.0

仅接收（RX-only）的 SDRuno POCSAG 与实验性铁路消息解析插件。当前发布包适用于已验证的 SDRuno 1.41.1（32 位插件接口）环境。

## 文件布局

请不要拆散这些文件：

```text
SDRunoPlugin_POCSAG.dll
RailwayMapServer.exe
web/
  index.html
  app.js
  style.css
```

`RailwayMapServer.exe` 必须能在自身旁边找到完整 `web/`。

## 安装

1. 完全退出 SDRuno。
2. 将上述文件复制到 SDRuno `Plugin Control` 使用的 Community Plugin 目录；推荐 `%USERPROFILE%\Documents\CommunityPlugins\`。
3. 启动 SDRuno，在 `Plugin Control` 中加载插件。
4. 插件内点击 `打开实时地图`，或访问 <http://127.0.0.1:8765/>。

## 默认接收参数

- 频率：821.2375 MHz
- 模式：NFM
- 带宽：15 kHz
- 速率：1200 baud

地图在缺少有效坐标时不会创建 `(0,0)` 标记；缺少线路字段时默认使用“沪昆线”，并允许在网页端按目标手动修改。

如提示缺少 `MSVCP140.dll` 或 `VCRUNTIME140.dll`，请安装 Microsoft Visual C++ 2015–2022 Redistributable（x86）。本项目是实验研究工具，不得作为铁路安全或作业防护设备。

完整文档：<https://github.com/WendyAi2005/SDRunoPlugin_POCSAG/tree/release/v1.0.0>
