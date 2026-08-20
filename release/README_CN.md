# SDRuno POCSAG 中国铁路监测插件 v1.0.0

适用环境：SDRuno 1.41.1（32 位插件接口）。

## 安装

1. 完全退出 SDRuno。
2. 将 `SDRunoPlugin_POCSAG.dll`、`RailwayMapServer.exe` 和 `web` 文件夹复制到：
   `%USERPROFILE%\Documents\CommunityPlugins\`
3. 启动 SDRuno，在 `Plugin Control` 中加载 `POCSAG`。
4. 插件内点击“打开实时地图”，或访问 `http://127.0.0.1:8765/`。

## 默认接收参数

- 频率：821.2375 MHz
- 模式：NFM
- 带宽：15 kHz
- 速率：1200 baud

地图在缺少有效坐标时不会创建 `(0,0)` 标记；缺少线路字段时默认使用“沪昆线”，并允许在网页端按目标手动修改。
