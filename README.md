# VeloWebView

VeloWebView is a native, multi-instance WebView UMG widget for Unreal Engine.
It keeps the web page in the platform browser compositor instead of reading a
browser bitmap back to a `UTexture2D` every frame.

## Supported targets

- Unreal Engine 5.4 (the project currently uses 5.4.4; the code is written for
  the UE 5.4+ API surface).
- Windows Editor and packaged Win64 builds through Microsoft Edge WebView2.
- Android builds through `android.webkit.WebView` and a native view overlay.

The core module is platform-neutral. `VeloWebViewWindows` and
`VeloWebViewAndroid` are loaded through `IVeloWebViewPlatformModule`; UMG and
Blueprint code never includes WebView2 or JNI types.

## Installation

1. Enable `VeloWebView` in the project.
2. Re-generate project files after changing the plugin descriptor.
3. Build the target once so UHT generates the widget reflection data.
4. For Android, package with the Android target so the UPL copies the Java
   manager and adds the manifest/lifecycle hooks.

The plugin does not enable `WebBrowserWidget`, `CEF3`, or the UE WebBrowser
module.

## Windows WebView2 dependency

The plugin uses the official Microsoft SDK and does not use CEF. The SDK was
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
The end-user machine still needs the Evergreen WebView2 Runtime. The plugin
never disables certificate validation and never silently substitutes CEF.

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
Set `Initial URL`, JavaScript, autoplay, muted and transparent-background
options, then use:

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

Windows handles WebView2 `NewWindowRequested`. Android enables multiple-window
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

All native callbacks are copied through weak UObject lambdas and dispatched to
the UE game thread before Blueprint delegates are broadcast.

## JavaScript bridge

The page-facing API is identical on Windows and Android:

```javascript
window.VeloWebView.postMessage("hello");
window.VeloWebView.postMessage(JSON.stringify({event:"VideoStarted", id:"10086"}));
```

Use `OnJavaScriptMessage` in Blueprint/C++ to receive the string. UE-to-page
calls use `ExecuteJavaScript`, for example
`window.player && window.player.play();`. The bridge does not expose arbitrary
UObjects or native function invocation.

## Video and live playback

The plugin does not implement a decoder. A normal HTML5 page can use
`<video controls autoplay muted playsinline>`, MP4/H.264, audio, HLS (`hls.js`),
DASH (`dash.js`/Shaka), WebRTC and HTTP-FLV/mpegts.js subject to the selected
WebView2 Runtime, Android System WebView, OS codecs, DRM and server CORS/HTTPS
configuration. Android fullscreen is handled through
`WebChromeClient.onShowCustomView/onHideCustomView`; Windows reports
`ContainsFullScreenElementChanged`.

## Security

Mixed content is disabled by default. SSL errors are not ignored, arbitrary
file access is not enabled, and sensitive permissions are not auto-granted.
Android maps `SetAllowMixedContent` to WebView's mixed-content setting; on
Windows WebView2 has no process-wide switch, so the secure policy remains in
effect and applications should explicitly handle trusted resource URLs.
Use the permission event and explicitly call `GrantPermission` only after the
application has made its own decision.

## Known limitations

- WebView2 SDK/runtime and Android device runtime were not available for an
  interactive browser test in this environment. The Windows SDK-enabled branch
  is implemented but not runtime-verified here.
- Native browser overlays have rectangular clipping and platform-specific
  Z-order. Slate widgets/materials cannot reliably render above the Android
  WebView, and Windows child-controller composition has similar native-window
  ordering constraints. Transparent backgrounds also depend on the WebView2
  runtime/controller support and fall back to an opaque surface when needed.
- Windows reports WebView2's fullscreen-element state; a product that wants a
  borderless app-wide fullscreen surface should use `OnFullscreenChanged` to
  adjust its own host layout, because native child-window Z-order is controlled
  by the host window.
- V1 intentionally does not render pages into `UTexture2D` or support
  `WidgetComponent`/3D widgets.
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
- **Android overlay is offset**: verify device density/edge-to-edge settings;
  V1 uses a rectangular content overlay and converts Slate geometry through the
  display density.
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
- `EmptyProject Android Development`: native arm64 compilation and UPL staging
  passed. The final Gradle step was attempted but failed in this environment
  with `java.io.IOException: Unable to establish loopback connection` from the
  installed Gradle/JVM daemon; this is an environment packaging failure, not a
  C++/JNI/UPL compile error. The generated Java manager was independently
  compiled with Android API 29.
- Java manager was independently compiled with Android API 29 (`javac` exit 0;
  one expected deprecation warning for `CookieSyncManager`).
