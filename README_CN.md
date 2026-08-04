# SDRuno POCSAG 列车接近预警接收插件

这是一个只接收的 SDRuno 社区插件。它直接读取 SDRuno 内部的 NFM 音频流并解码 POCSAG，不需要 VB-CABLE、PDW，也不包含任何发射功能。

> 当前为实验测试版。解码器已通过模拟 POCSAG 1200 数字消息和 `TONE ONLY` 呼叫测试，并参考 PDW 的音频零交叉锁相思路增强了现场信号同步；不同接收机、音频链和信号质量仍可能需要继续调试。

## 默认用途

- 接收频率：821.237500 MHz
- 调制：NFM
- 滤波带宽：15 kHz
- 默认速率：POCSAG 1200 baud
- 支持速率：512 / 1200 / 2400 baud
- 输出：时间、RIC 地址、功能位、Numeric/Alpha 消息、BCH 纠错位数

## 安装

1. 关闭 SDRuno。
2. 将 `release\SDRunoPlugin_POCSAG.dll` 复制到：
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

生成文件位于 `build\SDRunoPlugin_POCSAG.dll`。

## 开源参考

- SDRuno 插件接口：SDRplay 官方 Plugins SDK
- 音频同步思路参考：开源 [PDW Paging Decoder](https://github.com/Discriminator/PDW)
