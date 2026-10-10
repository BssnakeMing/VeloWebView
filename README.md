# VeloWebView

VeloWebView is a multi-instance WebView UMG widget for Unreal Engine. Its
default **Composited With UMG** mode draws the page within Slate, so sibling
widgets and UMG ZOrder can cover it. **Native Overlay** preserves the original
platform browser presentation when native video/permission behavior matters.

## Supported targets

- Unreal Engine 5.4 (the project currently uses 5.4.4; the code is written for
  the UE 5.4+ API surface).
- Windows Editor and packaged Win64 builds through an out-of-process CEF
  BrowserHost in composited mode. Native Overlay remains the legacy WebView2
  compatibility path and is not used by Composited.
- Android builds through the engine's WebView texture path in composited mode,
  or `android.webkit.WebView` in a native overlay.

The core module is platform-neutral. `VeloWebViewWindows` and
`VeloWebViewAndroid` are loaded through `IVeloWebViewPlatformModule`; UMG and
Blueprint code never includes WebView2 or JNI types.

## Installation

1. Enable `VeloWebView` in the project.
2. Re-generate project files after changing the plugin descriptor.
3. Build the target once so UHT generates the widget reflection data.
4. For Android, package with the Android target so the UPL copies the Java
   manager and adds the manifest/lifecycle hooks.

On Win64, Composited mode does not load UE's `WebBrowser` module or `libcef.dll`
inside the editor/game. It starts `BrowserHost.exe` and uses the versioned
named-pipe/shared-memory protocol documented in `Docs/IPC_PROTOCOL.md`.
The BrowserHost is staged with the CEF DLLs, pak files and `locales` directory
when the CEF package and host executable are present. The initial renderer
consumes the CPU fallback ring; the D3D12 shared-texture bridge remains the
high-performance path under development.
Android keeps the engine WebBrowser implementation. Run
`Scripts/SetupCEF.ps1` to download the pinned CEF 154 Windows x64 package; the
runtime is intentionally kept out of Git.

### CEF runtime download

The source repository keeps the large CEF distribution out of Git. For a
ready-to-use Windows package, download the CEF 154 archive from the project
share and extract it into the plugin's `ThirdParty/CEF` directory (or follow
the archive's included layout):

- [CEF_154.0.32_g682c378_chromium-154.0.8037.58_windows64_UE_WebBrowser.zip](https://1823780428.share.123pan.cn/123pan/MRgYjv-vIFOH?pwd=9lfo)
- 123 云盘提取码：`9lfo`

The compiled `VeloWebView-1.0.0-Win64.zip` package is published on the
GitHub Releases page. It contains the packaged plugin and its Win64 runtime;
the external CEF archive is also provided for source builds and repackaging.
See [`Docs/CEF_DOWNLOAD.md`](Docs/CEF_DOWNLOAD.md) for the expected layout and
license notes.

## Windows WebView2 dependency

Native Overlay mode uses the official Microsoft WebView2 SDK. The SDK was
staged in this checkout from the official NuGet package; the Build.cs still
keeps a clear runtime-error fallback (`WebView2 Runtime/SDK not installed`)
for source distributions where the SDK has not been staged. Install or
refresh it with:

```powershell
.\Scripts\Install-WebView2SDK.ps1
```

The script downloads the official `Microsoft.Web.WebView2` NuGet package and
copies its `WebView2.h`, x64 loader import/runtime files into
`ThirdParty/WebView2`. Review the package/license before committing binaries.
Native Overlay mode still requires the Evergreen WebView2 Runtime on the
end-user machine. Switching between modes is explicit in `Render Mode`.

The Windows adapter creates one environment/controller/WebView per widget,
with a unique saved WebView2 user-data directory per instance,
registers navigation, title, console, message, permission, download and
fullscreen callbacks, and removes event handlers before releasing COM objects.
It uses the GPU-backed WebView2 controller path; V1 does not perform a CPU
readback. Bounds are converted from Slate screen coordinates to the host client
area every Slate tick.

## Android requirements

- Android API/NDK versions supplied by the UE 5.4 project (the checked project
  built native arm64 code with NDK r25b/API 29).
- A system WebView/Chrome provider on the device.
- `INTERNET` is added by the UPL. Camera, microphone and location permissions
  are declared but are only granted after a WebView permission request and an
  Android runtime permission result.

`VeloWebViewManager` owns a map of instance IDs, creates views on the Android UI
thread, and places them in a transparent overlay above the UE content view.
JavaScript interface exposure is limited to `window.VeloWebView.postMessage`.
HTML video, audio, autoplay policy, HLS/DASH/WebRTC JavaScript players and
`WebChromeClient` fullscreen are delegated to the system WebView/media stack.

## Blueprint usage

Add **Velo Web View** from the `VeloWebView` category to a Widget Blueprint.
Set `Initial URL`, `Render Mode`, JavaScript, autoplay, muted and
transparent-background options, then use:

### Rendering and ZOrder

`Render Mode` defaults to **Composited With UMG**. Put Velo Web View below
buttons, panels, or dialogs in the same UMG hierarchy; ordinary UMG ZOrder now
controls their visual and input order. On Windows this mode uses the engine's
offscreen CEF browser. On Android it gives the engine WebView a virtual parent
so it paints a browser texture through Slate. A runtime test is available in
Development builds: run `VeloWebView.LayerTest` in the console; it adds a
full-screen browser and a green UMG bar above it and writes
`VeloWebViewLayerTest.png` to the game's Screenshots directory.

Windows Composited resolution is controlled by `Composited Resolution Scale`.
`1.0` renders at the widget's physical Slate size; `1.5` or `2.0` supersamples
the browser surface for sharper text and images at higher CPU memory and frame
upload cost. The frame ring is allocated from the first real Slate geometry and
is recreated automatically if a later window resize exceeds its capacity.

Choose **Native Overlay** to retain WebView2/Android's original direct view,
including its media and permission behavior. That mode cannot obey UMG ZOrder;
keep native WebView bounds outside UI that must appear in front.

Composited mode provides URL/HTML loading, navigation, JavaScript execution,
page/title/console events, new-window notification, and cookie writes/deletes.
`GetCookie` returns only values set through this widget. The engine browser
does not expose this plugin's per-instance user-agent, autoplay, mixed-content,
cache clearing, or permission decision controls; those methods apply to Native
Overlay mode. Video playback, DRM, text input, and frame rate can differ from
the native mode and should be checked on target devices.

- `LoadURL`, `LoadHTML`, `Reload`, `Stop`, `GoBack`, `GoForward`;
- `CanGoBack`, `CanGoForward`, `GetURL`, `GetTitle`, `GetState`, `IsReady`;
- `ExecuteJavaScript` and the JavaScript/media/background/user-agent setters;
- cookie/cache methods and explicit `GrantPermission`/`DenyPermission`.

The **Set Visibility** Blueprint node is implemented by
`SetWebViewVisibility(bool)` so it does not hide UWidget's existing
`SetVisibility(ESlateVisibility)` API.

Bind `OnPageStarted`, `OnPageLoaded`, `OnPageError`, `OnURLChanged`,
`OnTitleChanged`, `OnJavaScriptMessage`, `OnConsoleMessage`,
`OnDownloadRequested`, `OnPermissionRequested`, `OnFullscreenChanged` and
`OnBeforeNavigate` in the normal way.

### Links that request a new window

Ordinary links navigate in the current widget. For `target="_blank"` and
`window.open(url)`, **New Window Policy** defaults to **Open In Same View**,
so third-party pages can follow these links without creating an unmanaged
native window. The `OnNewWindowRequested(URL, bIsUserInitiated)` event reports
the request. It is a notification, not a synchronous decision callback.

The other policies are **Open External Browser**, **Notify Only** (handle the
URL in your own UI, for example by creating another Velo Web View widget),
and **Block**. Set the policy in the widget details or with
`SetNewWindowPolicy` before loading the page. `OnBeforeNavigate` remains
advisory and does not cancel navigation.

Native Windows handles WebView2 `NewWindowRequested`. Native Android enables multiple-window
requests and uses a short-lived child WebView to obtain the target URL from
`WebChromeClient.onCreateWindow`; it destroys that child after routing the URL.
The URL-based policies cover links and `window.open(url)`. A site that creates
an empty popup and writes HTML into it, relies on `window.opener`, or submits a
POST to a new window needs a real second browser view and is not represented by
one URL. `Notify Only` can build a simple separate tab from a URL, but it does
not preserve those popup relationships. Separate Windows widgets currently use
separate WebView2 data directories, so they do not share login cookies.
On Android, a blocked popup reports an empty URL because the system does not
expose its destination until a child WebView is created.
Composited Android also intercepts user clicks on `target="_blank"` links and
`window.open(url)` after document startup, so those common requests stay in
the selected policy instead of producing a native child view. Popups started
before script injection and POST submissions to a new window remain limited by
the engine browser backend.

## C++ usage

```cpp
UPROPERTY(meta=(BindWidget))
TObjectPtr<UVeloWebView> WebView;

void UMyWidget::NativeConstruct()
{
    Super::NativeConstruct();
    WebView->LoadURL(TEXT("https://example.com"));
}
```

Browser callbacks are dispatched to the UE game thread before Blueprint
delegates are broadcast.

## JavaScript bridge

The page-facing API in Native Overlay mode is identical on Windows and Android.
Composited mode installs the same `postMessage` shim after document loading:

```javascript
window.VeloWebView.postMessage("hello");
window.VeloWebView.postMessage(JSON.stringify({event:"VideoStarted", id:"10086"}));
```

Use `OnJavaScriptMessage` in Blueprint/C++ to receive the string. UE-to-page
calls use `ExecuteJavaScript`, for example
`window.player && window.player.play();`. The bridge does not expose arbitrary
UObjects or native function invocation.

## Host page and iframe

The plugin ships [`Content/Host.html`](Content/Host.html) as a local browser
host. It keeps controls that must stay above the embedded page inside the
browser itself and loads the product page in an iframe. The template contains
`退出` and `刷新` buttons and exposes `window.VeloHost`:

```cpp
// The helper reads Content/Host.html and sets the initial iframe URL.
WebView->LoadHostHTML(TEXT("https://example.com"));

// These calls can be made later, for example after receiving a UE message.
WebView->SetHostIframeURL(TEXT("https://example.com/next"));
WebView->RefreshHostIframe();
```

The equivalent JavaScript calls are
`window.VeloHost.setIframeURL(url)` and
`window.VeloHost.refreshIframe()`. UE can also send a `window.postMessage`
payload such as `{type:"set-iframe-url",url:"https://example.com"}`. The
host buttons send JSON messages through `window.VeloWebView.postMessage`; bind
`OnJavaScriptMessage` to handle the `exit`, `iframe-refresh` and
`iframe-loaded` commands.

The current Win64 Composited runtime is the custom CEF 154 build from the
standalone CEF project. Its H.264 capability page and a real H.264/AAC MP4
passed through the BrowserHost CPU offscreen-rendering path. The pinned commit,
codec arguments and replacement record are in `Docs/CEF_VERSION.md`; the
standalone build process is in `Docs/CEF_STANDALONE_BUILD.md` and the test page
is `Content/CodecTest.html`.

`Allow Media Capture` only grants trusted WebRTC camera/microphone access;
`Enable Pixel Streaming` adjusts autoplay/background throttling.

## Video and live playback

The plugin does not implement a decoder. A normal HTML5 page can use
`<video controls autoplay muted playsinline>`, HLS (`hls.js`), DASH
(`dash.js`/Shaka), WebRTC and HTTP-FLV/mpegts.js subject to the selected
browser runtime, OS codecs, DRM and server CORS/HTTPS configuration. H.264/MP4
and H.264 WebRTC require the separately built codec-enabled CEF runtime
described above. Android fullscreen is handled through
`WebChromeClient.onShowCustomView/onHideCustomView`; Windows reports
`ContainsFullScreenElementChanged`.

## Security

The original native adapters do not ignore SSL errors or automatically grant
sensitive permissions. Android Native Overlay maps `SetAllowMixedContent` to
WebView's mixed-content setting. Composited mode follows the engine browser's
security defaults; this plugin cannot apply its mixed-content and permission
methods there. Review that runtime's policy before loading untrusted pages.

## Known limitations

- The Windows WebView2 branch has not been interactively runtime-verified in
  this environment.
- Native Overlay mode has rectangular clipping and platform-specific Z-order.
  Slate widgets cannot render above it. Transparent backgrounds depend on the
  platform browser runtime and may fall back to an opaque surface.
- Windows reports WebView2's fullscreen-element state; a product that wants a
  borderless app-wide fullscreen surface should use `OnFullscreenChanged` to
  adjust its own host layout, because native child-window Z-order is controlled
  by the host window.
- Composited mode renders the browser through Slate. `WidgetComponent`/3D
  widgets remain unsupported by this plugin.
- `GetCookie` is synchronous at the UE API boundary. WebView2's native cookie
  manager is asynchronous, so the Windows adapter returns its local cache; use
  page JavaScript for authoritative asynchronous cookie reads when needed.
- Android Activity recreation depends on the UE GameActivity lifecycle hooks;
  test the target project's navigation/rotation policy before shipping.
- Navigation interception is advisory (`OnBeforeNavigate`); the current public
  event does not yet return an allow/cancel decision.

## Troubleshooting

- **WebView2 Runtime not installed**: install the Evergreen Runtime and confirm
  `WebView2Loader.dll` is staged beside the packaged executable.
- **Windows widget is blank**: verify the SDK was copied to
  `ThirdParty/WebView2/include`, regenerate project files, and check
  `LogVeloWebViewWindows` for the HRESULT.
- **Android overlay is offset**: compare the UE logical viewport with the
  Android SurfaceView bounds. The plugin maps Slate coordinates to physical
  SurfaceView pixels; Android display density is only used by explicitly
  dp-based calls.
- **No JS messages**: ensure the page calls exactly
  `window.VeloWebView.postMessage(...)` and that JavaScript is enabled.
- **Video does not autoplay**: browser policy and page `muted`/`playsinline`
  attributes still apply; use a user gesture when the platform requires one.

## Packaging checklist

### Windows

- Stage `WebView2Loader.dll` from `ThirdParty/WebView2/bin/x64`.
- Install Evergreen WebView2 Runtime on the target machine.
- Test PIE start/stop repeatedly and verify the packaged executable does not
  depend on an editor plugin path.

### Android

- Keep the UPL enabled for the Android target; it copies
  `Java/src/com/velowebview/VeloWebViewManager.java` and adds lifecycle hooks.
- Confirm the generated manifest contains only the permissions your product
  needs, and test camera/microphone runtime prompts on the target API level.
- Verify your device has a current Android System WebView provider.

## Build verification for this checkout

- `EmptyProjectEditor Win64 Development`: passed with UE 5.4.4 UHT, C++ compile
  and link (including the no-SDK Windows fallback).
- `EmptyProject Android Development -SkipDeploy`: native arm64 compilation and
  linking passed. Gradle `:app:assembleDebug` passed with
  `-Dorg.gradle.jvmargs=` to avoid this host's single-use daemon loopback error.
  The resulting APK was installed on a 1920x1080 Android emulator via ADB
  port 5555. Its WebView bounds matched the full 1920x1080 SurfaceView; the
  previous APK's WebView measured 1280x720.
