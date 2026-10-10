# VeloWebView CEF 浏览器项目说明

## 一、项目定位

本项目面向 Unreal Engine 5.4 Windows x64，建设一个独立于 Unreal Editor 和
Game 进程的 CEF 浏览器运行方案。

当 `UVeloWebView::RenderMode` 为 `Composited` 时，网页由独立的
`BrowserHost.exe` 进程承载，UE 侧通过 IPC 和 GPU 纹理桥接将网页呈现到 UMG。
该方案服务于高分辨率网页、复杂 JavaScript、WebGL、HTML5 视频以及 WebRTC
Pixel Streaming 等场景。

## 二、主管目标

### 主要目标

1. 摆脱 UE 进程内 CEF、WebView2 和 Microsoft Edge SDK 的运行时依赖。
2. 使用官方 CEF Windows x64 发行版，当前基线为 CEF 154 / Chromium 154。
3. 通过 CEF Windowless / Off-Screen Rendering 实现 Composited WebView。
4. 优先采用 `OnAcceleratedPaint` 和 Windows shared texture，避免每帧
   `GPU -> CPU -> GPU` 像素拷贝。
5. 建立版本化、可校验、支持多实例的 BrowserHost 与 UE IPC 协议。
6. 为 H.264、WebRTC 和 Pixel Streaming 保留可落地的自定义 CEF 构建路径。
7. 保持现有 Blueprint WebView API 的兼容性，降低业务迁移成本。

### 业务结果

- UE 项目可以在不安装 Edge/WebView2 Runtime 的环境中运行 CEF Composited WebView。
- 网页可以显示在 UMG 层级中，并支持鼠标、键盘、焦点、缩放和窗口尺寸变化。
- BrowserHost 异常退出时，UE 不崩溃，并能向业务层报告浏览器故障。
- 复杂网页和 WebRTC 页面不会因为浏览器像素 CPU 上传导致明显 Game Thread 峰值。
- H.264 WebRTC Pixel Streaming 能够通过经过许可审查的 CEF/Chromium 构建交付。

## 三、项目范围

### 第一阶段范围

- Windows x64
- Unreal Engine 5.4
- DirectX 12 UE RHI
- CEF Windowless OSR
- CEF `shared_texture_enabled`
- `OnAcceleratedPaint`
- BrowserHost 独立进程
- Named Pipe 控制协议
- Shared Memory 帧元数据和 CPU fallback ring
- URL 导航、HTML 加载、JavaScript、刷新、前进、后退、Resize、Visibility、Focus
- 基础 WebRTC 媒体权限和 Pixel Streaming 浏览器启动参数

### 明确不纳入第一阶段

- RTSP DOM Element
- UE 内部直接加载 `libcef.dll`
- Linux、macOS、Android CEF GPU 纹理桥
- Vulkan 纹理桥
- 完整 IME 中文输入
- H.264 专有编解码器的默认二进制分发
- RTSP Media Subsystem

## 四、技术方案

```text
UMG / Slate
    |
    | BrowserProtocol: Named Pipe + Shared Memory
    | GPU texture handle / frame metadata
    v
BrowserHost.exe
    |
    | CEF Windowless / OSR
    | OnAcceleratedPaint
    | CEF renderer / GPU subprocess
    v
Chromium
```

技术原则如下：

- `libcef.dll` 只能由 `BrowserHost.exe` 加载。
- UE 与 BrowserHost 之间的消息必须带 magic、version、size、instance id 和 sequence。
- CEF 临时 shared texture handle 不能跨 callback 缓存，必须在 callback 内复制到
  BrowserHost 自有纹理 ring。
- 正常 GPU 路径使用 GPU-to-GPU copy；CPU OnPaint 仅作为诊断和兼容 fallback。
- 共享资源、fence、slot 和 resize 状态必须避免 tearing 和竞态。
- 所有浏览器实例必须使用独立的 instance id 和 IPC 名称。

## 五、WebRTC 与 H.264 策略

官方 CEF binary 作为开源编解码基线使用。VP8、VP9、AV1 等能力需要在目标页面和
目标 GPU 驱动上验证；H.264/AAC 不能默认假设可用。

H.264 WebRTC Pixel Streaming 的交付路径为：

1. 使用 `Scripts/BuildCEFWithCodecs.ps1` 构建匹配的 CEF/Chromium。
2. 配置 `proprietary_codecs=true`、`ffmpeg_branding="Chrome"`、`rtc_use_h264=true`。
3. 记录 CEF 版本、Chromium commit、GN 参数和 SHA256。
4. 完成专利、编码器和发行许可审查。
5. 通过 WebRTC SDP 和 `RTCRtpSender.getCapabilities('video')` 验证 H.264。
6. 在目标 Pixel Streaming 信令服务器、编码器和显卡环境中完成端到端测试。

`Allow Media Capture` 只控制可信页面的摄像头/麦克风授权；`Enable Pixel Streaming`
只调整自动播放和后台节流策略，两者都不会自动增加 H.264 编解码能力。

## 六、里程碑

### M1：BrowserHost 独立运行

- CEF 初始化成功。
- 可打开指定 URL。
- 可创建 Windowless Browser。
- `OnAcceleratedPaint` 和 `OnPaint` fallback 均能产生帧元数据。

### M2：IPC 与帧 Ring

- Named Pipe 握手成功。
- Frame Header 能被校验和读取。
- 三槽帧 Ring 能处理 sequence、slot、dirty rect 和 resize。
- BrowserHost 被关闭或 kill 后，UE 能识别异常。

### M3：UE Composited 集成

- `RenderMode=Composited` 自动启动 BrowserHost。
- URL、HTML、JavaScript 和导航命令可从 UE 发送。
- BrowserHost 不被加载进 UE 进程。

### M4：D3D12 GPU 纹理桥

- UE 通过 `OpenSharedHandle` 接收 BrowserHost 自有 shared texture。
- 使用 UE-owned texture 和 GPU copy 呈现到 Slate/UMG。
- 正常路径不执行全帧 BGRA CPU 上传。

### M5：交互与稳定性

- 鼠标、键盘、滚轮、焦点、DPI、Resize 可用。
- PIE 重启、Editor 退出和 BrowserHost kill 不产生孤儿进程或 UE 崩溃。

### M6：WebRTC / Pixel Streaming 验证

- VP8/VP9/AV1 能力验证完成。
- H.264 自定义 CEF 构建完成并通过许可审查。
- Pixel Streaming 完成浏览器、信令、编码器和 GPU 端到端验证。

## 七、验收标准

- 能打开 Google 或等效复杂 JavaScript 页面。
- WebGL 页面能够正常渲染。
- UMG 控件可以覆盖在网页上方。
- Widget 调整尺寸后浏览器同步 Resize。
- UE DX12 模式下网页纹理可显示。
- BrowserHost 被 kill 后，UE 继续运行并报告错误。
- 1920x1080 / 60 FPS 页面运行时无明显 CPU BGRA 全帧上传峰值。
- H.264 WebRTC Pixel Streaming 的 SDP 协商包含可用 H.264 codec，并能持续播放。
- Shipping 包含正确的 BrowserHost、CEF DLL、resources 和 locales。
- 没有安装 Edge/WebView2 Runtime 时，Composited CEF 模式仍能启动。

## 八、主要风险与控制措施

| 风险 | 影响 | 控制措施 |
| --- | --- | --- |
| 官方 CEF binary 不含 H.264/AAC | MP4 或 H.264 Pixel Streaming 失败 | 自定义 CEF 构建、能力探测、许可审查 |
| CEF shared handle 生命周期短 | 黑屏、崩溃、纹理竞态 | callback 内立即 Open/Copy，使用 Host-owned ring |
| D3D11/D3D12 fence 互操作差异 | 撕裂或等待失败 | fence value、slot 状态、Flush fallback 和驱动测试 |
| BrowserHost 异常退出 | 页面消失或资源泄漏 | IPC disconnect 检测、错误事件、重启接口 |
| CEF 版本升级破坏 API | 构建或运行时不兼容 | 固定版本清单、协议隔离、升级回归测试 |
| 专有 codec 许可不清晰 | 无法发布 | 在自定义构建前完成法律和供应链审查 |

## 九、当前状态

当前已完成 BrowserHost 工程骨架、CEF 154 下载校验脚本、共享协议、Named Pipe
控制端、UE Win64 Composited 外部连接和 WebRTC 启动参数。UE5.4 Win64 编译已通过。

当前未完成最终 UE D3D12 shared texture bridge，因此现阶段结果属于架构和 IPC
里程碑，不应被描述为已经完成零拷贝 GPU 浏览器渲染或 H.264 Pixel Streaming 交付。

## 十、项目管理要求

- 每次 CEF 升级必须更新 `Docs/CEF_VERSION.md`、校验值和兼容性记录。
- 每个里程碑必须有可重复的构建命令和验收日志。
- 第三方代码必须先检查许可证，再决定是否引入。
- 不提交 CEF 大型二进制文件到 Git。
- 任何 H.264/AAC 发行版本必须附带 codec 许可结论。
- 发现 GPU 纹理桥、CEF 进程或 WebRTC 协商存在未验证项时，必须在发布说明中明确标注。
