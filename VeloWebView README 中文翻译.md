# VeloWebView

VeloWebView 是一个面向 Unreal Engine 的原生、多实例 WebView UMG 控件。

它让网页继续由平台浏览器的合成器负责渲染，而不是每一帧都把浏览器位图读回到 `UTexture2D` 中。

## 支持的平台

- Unreal Engine 5.4（当前项目使用 5.4.4；代码基于 UE 5.4+ API 编写）。
- Windows Editor，以及通过 Microsoft Edge WebView2 构建的 Win64 打包版本。
- Android 构建，通过 `android.webkit.WebView` 和原生 View 覆盖层实现。

核心模块与平台无关。`VeloWebViewWindows` 和 `VeloWebViewAndroid` 通过 `IVeloWebViewPlatformModule` 加载；UMG 和 Blueprint 代码不会包含 WebView2 或 JNI 类型。

## 安装

1. 在项目中启用 `VeloWebView`。
2. 修改插件描述文件后，重新生成项目文件。
3. 至少构建一次目标，以便 UHT 生成控件的反射数据。
4. 对于 Android，请使用 Android Target 进行打包，以便 UPL 复制 Java Manager，并添加 Manifest / 生命周期钩子。

该插件不会启用 `WebBrowserWidget`、`CEF3` 或 UE WebBrowser 模块。

## Windows WebView2 依赖

插件使用微软官方 SDK，不使用 CEF。当前代码检出版本中的 SDK 来自官方 NuGet 包；`Build.cs` 仍然保留了明确的运行时错误回退：

`WebView2 Runtime/SDK not installed`

这样，在未包含 SDK 的源码分发版本中可以给出明确提示。

安装或更新 SDK：

```powershell
.\Scripts\Install-WebView2SDK.ps1
```

该脚本会下载官方 `Microsoft.Web.WebView2` NuGet 包，并将其中的 `WebView2.h`、x64 Loader 导入/运行时文件复制到 `ThirdParty/WebView2`。

提交二进制文件之前，请检查对应的软件包及许可证。

最终用户的电脑仍然需要安装 Evergreen WebView2 Runtime。插件不会禁用证书验证，也不会在后台静默切换到 CEF。

Windows Adapter 会为每个 Widget 创建独立的 Environment、Controller 和 WebView，并为每个实例使用唯一的 WebView2 用户数据保存目录。

它会注册以下回调：

- 页面导航
- 标题
- Console
- 消息
- 权限
- 下载
- 全屏

释放 COM 对象之前，会先移除事件处理器。

插件使用 GPU 支持的 WebView2 Controller 路径；V1 不执行 CPU Readback。

每次 Slate Tick 时，Bounds 都会从 Slate 屏幕坐标转换到宿主窗口的 Client Area。

## Android 要求

- 使用 UE 5.4 项目所提供的 Android API / NDK 版本（当前检出的项目使用 NDK r25b / API 29 成功构建了原生 arm64 代码）。
- 设备上需要存在 System WebView / Chrome Provider。
- UPL 会添加 `INTERNET` 权限。Camera、Microphone 和 Location 权限虽然会被声明，但只有在 WebView 发出权限请求，并且 Android Runtime Permission 返回结果之后才会真正授予。

`VeloWebViewManager` 维护一个 Instance ID Map，在 Android UI Thread 上创建 View，并将其放置到 UE Content View 上方的透明 Overlay 中。

JavaScript Interface 仅暴露：

`window.VeloWebView.postMessage`

HTML Video、Audio、Autoplay Policy、HLS/DASH/WebRTC JavaScript Player，以及 `WebChromeClient` 全屏功能，都交由系统 WebView / Media Stack 处理。

## Blueprint 使用方法

在 Widget Blueprint 中，从 `VeloWebView` 分类添加 **Velo Web View**。

设置：

- `Initial URL`
- JavaScript
- Autoplay
- Muted
- Transparent Background

然后可以使用：

- `LoadURL`、`LoadHTML`、`Reload`、`Stop`、`GoBack`、`GoForward`
- `CanGoBack`、`CanGoForward`、`GetURL`、`GetTitle`、`GetState`、`IsReady`
- `ExecuteJavaScript`，以及 JavaScript / Media / Background / User-Agent Setter
- Cookie / Cache 方法，以及显式的 `GrantPermission` / `DenyPermission`

Blueprint 中的 **Set Visibility** 节点由：

`SetWebViewVisibility(bool)`

实现，这样就不会隐藏 UWidget 已有的：

`SetVisibility(ESlateVisibility)`

API。

可以正常绑定以下事件：

- `OnPageStarted`
- `OnPageLoaded`
- `OnPageError`
- `OnURLChanged`
- `OnTitleChanged`
- `OnJavaScriptMessage`
- `OnConsoleMessage`
- `OnDownloadRequested`
- `OnPermissionRequested`
- `OnFullscreenChanged`
- `OnBeforeNavigate`

## 新窗口链接

普通链接直接在当前控件跳转。对于 `target="_blank"` 和
`window.open(url)`，**New Window Policy** 默认是 **Open In Same View**，
因此第三方网页的这类链接会在当前 WebView 中打开。插件还会广播
`OnNewWindowRequested(URL, bIsUserInitiated)`；该事件用于通知，不能同步改变本次决策。

另外可以选择 **Open External Browser**、**Notify Only**（由蓝图根据 URL
创建自己的标签页）或 **Block**。可以在控件属性中设置，也可以在加载页面前调用
`SetNewWindowPolicy`。`OnBeforeNavigate` 仍然只负责通知，不能取消导航。

Windows 使用 WebView2 的 `NewWindowRequested`；Android 在
`WebChromeClient.onCreateWindow` 创建短暂的子 WebView，取得目标 URL 后销毁。
基于 URL 的处理适用于普通新窗口链接和 `window.open(url)`。如果网站先打开空白窗口，
再向它写入 HTML，依赖 `window.opener`，或把 POST 表单提交到新窗口，就需要真正的
第二个浏览器视图；仅转发 URL 无法保留这些关系。Windows 当前每个控件使用独立的
WebView2 数据目录，蓝图自行创建的多个控件不会共享登录 Cookie。
Android 的 **Block** 策略会在创建子 WebView 前拒绝请求，因此事件中的 URL 为空。

## C++ 使用方法

```cpp
UPROPERTY(meta=(BindWidget))
TObjectPtr<UVeloWebView> WebView;

void UMyWidget::NativeConstruct()
{
    Super::NativeConstruct();
    WebView->LoadURL(TEXT("https://example.com"));
}
```

所有 Native Callback 都会通过使用 Weak UObject 的 Lambda 进行复制，然后 Dispatch 到 UE Game Thread，之后才会广播 Blueprint Delegate。

## JavaScript Bridge

面向网页的 API 在 Windows 和 Android 上完全一致：

```javascript
window.VeloWebView.postMessage("hello");
window.VeloWebView.postMessage(JSON.stringify({event:"VideoStarted", id:"10086"}));
```

在 Blueprint / C++ 中使用 `OnJavaScriptMessage` 接收字符串。

从 UE 调用网页时使用 `ExecuteJavaScript`，例如：

```javascript
window.player && window.player.play();
```

该 Bridge 不会暴露任意 UObject，也不会提供任意 Native Function 调用能力。

## 视频与直播播放

插件本身不实现 Decoder。

普通 HTML5 页面可以使用：

```html
<video controls autoplay muted playsinline>
```

以及：

- MP4 / H.264
- Audio
- HLS（`hls.js`）
- DASH（`dash.js` / Shaka）
- WebRTC
- HTTP-FLV / mpegts.js

具体能否工作取决于所使用的 WebView2 Runtime、Android System WebView、操作系统 Codec、DRM，以及服务器端 CORS / HTTPS 配置。

Android 全屏通过：

`WebChromeClient.onShowCustomView/onHideCustomView`

处理。

Windows 则报告：

`ContainsFullScreenElementChanged`

## 安全

默认禁用 Mixed Content。

SSL Error 不会被忽略，不会启用任意文件访问，并且敏感权限不会自动授予。

Android 会把 `SetAllowMixedContent` 映射到 WebView 的 Mixed Content 设置。

Windows WebView2 没有 Process-Wide Switch，因此会继续维持安全策略；应用应当显式处理可信的资源 URL。

应使用 Permission Event，并且只有在应用自身完成权限判断后，才显式调用 `GrantPermission`。

## 已知限制

- 当前环境中没有可用于交互式浏览器测试的 WebView2 SDK / Runtime 和 Android Device Runtime。Windows 的 SDK-enabled 分支已经实现，但尚未在这里完成 Runtime 验证。
- Native Browser Overlay 使用矩形裁剪，并存在平台相关的 Z-order 限制。Slate Widget / Material 无法可靠地渲染在 Android WebView 上方；Windows Child Controller Composition 也存在类似的 Native Window 层级限制。透明背景同样取决于 WebView2 Runtime / Controller 是否支持；必要时会回退为不透明 Surface。
- Windows 会报告 WebView2 的 Fullscreen Element 状态。如果产品希望实现整个应用范围的无边框全屏 Surface，应监听 `OnFullscreenChanged` 并调整自己的 Host Layout，因为 Native Child Window 的 Z-order 由 Host Window 控制。
- V1 有意不把网页渲染到 `UTexture2D`，也不支持 `WidgetComponent` / 3D Widget。
- `GetCookie` 在 UE API 边界上是同步的。WebView2 原生 Cookie Manager 是异步的，因此 Windows Adapter 返回的是本地 Cache。如果需要权威的异步 Cookie 读取，应使用网页 JavaScript。
- Android Activity Recreation 依赖 UE GameActivity Lifecycle Hook；正式发布前，应测试目标项目的 Navigation / Rotation Policy。
- Navigation Interception 目前只是 Advisory（`OnBeforeNavigate`）；当前 Public Event 尚不能返回 Allow / Cancel 决策。

## 故障排查

- **WebView2 Runtime not installed**：安装 Evergreen Runtime，并确认 `WebView2Loader.dll` 已被放置在打包后的可执行文件旁。
- **Windows Widget 空白**：确认 SDK 已复制到 `ThirdParty/WebView2/include`，重新生成项目文件，并检查 `LogVeloWebViewWindows` 中的 HRESULT。
- **Android Overlay 发生偏移**：检查 Device Density / Edge-to-Edge 设置；V1 使用矩形 Content Overlay，并通过 Display Density 转换 Slate Geometry。
- **收不到 JS Message**：确认页面调用的准确形式为 `window.VeloWebView.postMessage(...)`，并确认 JavaScript 已启用。
- **视频无法自动播放**：浏览器策略以及页面的 `muted` / `playsinline` 属性仍然生效；如果平台要求用户手势，则需要通过 User Gesture 启动播放。

## 打包检查清单

### Windows

- 将 `ThirdParty/WebView2/bin/x64` 中的 `WebView2Loader.dll` 一并打包。
- 在目标机器上安装 Evergreen WebView2 Runtime。
- 反复测试 PIE Start / Stop，并确认打包后的 EXE 不依赖 Editor Plugin Path。

### Android

- Android Target 应保持 UPL 启用；它会复制：
  `Java/src/com/velowebview/VeloWebViewManager.java`
  并添加 Lifecycle Hook。
- 确认生成的 Manifest 只包含产品真正需要的权限，并在目标 API Level 上测试 Camera / Microphone Runtime Permission Prompt。
- 确认设备安装了较新的 Android System WebView Provider。

## 当前代码版本的构建验证

- `EmptyProjectEditor Win64 Development`：已通过 UE 5.4.4 UHT、C++ 编译和链接，包括没有 SDK 时的 Windows Fallback。
- `EmptyProject Android Development`：原生 arm64 编译以及 UPL Staging 已通过。最终 Gradle 步骤已经尝试执行，但在当前环境中失败，错误为：

```text
java.io.IOException: Unable to establish loopback connection
```

该错误来自已安装的 Gradle / JVM Daemon，属于环境打包问题，而不是 C++ / JNI / UPL 编译错误。

生成的 Java Manager 已单独使用 Android API 29 成功编译。

- Java Manager 也已单独使用 Android API 29 编译成功（`javac` Exit Code 为 0）；仅出现一个预期中的 `CookieSyncManager` Deprecated Warning。
