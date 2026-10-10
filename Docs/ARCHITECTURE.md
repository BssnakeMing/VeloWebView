# VeloWebView CEF architecture

Windows `RenderMode=Composited` is split into three processes/boundaries:

```text
UMG/Slate (VeloWebView module)
        | named pipe: versioned control/input/events
        | named file mapping: frame metadata + CPU fallback ring
        | named GPU handles: future D3D12 texture bridge
BrowserHost.exe (CEF windowless browser)
        | CEF renderer/GPU subprocesses
```

`libcef.dll` is loaded only by `BrowserHost.exe`; the Unreal Editor and game do
not load it. `BrowserHost` owns CEF initialization, browser lifetime,
windowless rendering, popup state, navigation events and crash detection. The
plugin owns the connection, Slate input mapping, lifecycle and Blueprint
delegates. `Shared/BrowserProtocol/BrowserProtocol.h` is the only shared ABI.

The normal GPU path reports `OnAcceleratedPaint` metadata and is designed for a
D3D12 `OpenSharedHandle` bridge. The current first consumer also supports an
explicit CPU `OnPaint` fallback through a three-slot ring, which makes the
process/IPC milestone testable on machines without a working CEF GPU adapter.
The fallback is not the intended 60 FPS production path.

All names include the per-widget instance id. A dead host is treated as an
input/IPC error and reported to the widget; it never terminates the UE process.
