# IPC protocol

The protocol is defined in `Shared/BrowserProtocol/BrowserProtocol.h`. Every
message starts with `FMessageHeader` and is rejected unless `magic`, protocol
version, struct size and payload size are valid. Frame mappings start with
`FFrameRingHeader` and contain three `FFrameSlot` records followed by pixel
storage. Producers publish metadata before the sequence number; consumers read
the sequence twice to avoid tearing during resize or slot reuse.

Control messages use a named pipe whose name is derived from the instance id.
Large pixel buffers never travel through the pipe. Frame metadata contains
width, height, format, slot, frame id, sequence, timestamp, dirty rectangle and
flags. `FrameGpuSharedTexture` marks a host-owned shared-handle frame;
`FrameCpuFallback` marks an `OnPaint` frame. The transient handle supplied by
CEF is never published directly because CEF may recycle it as soon as
`OnAcceleratedPaint` returns.

The command set covers create/destroy, URL/HTML navigation, back/forward,
reload/stop, resize, focus/visibility, frame rate, JavaScript, devtools and
shutdown. Browser events including navigation, title, console and JavaScript
messages return through the same pipe. IPC protocol version 1 transports text
and JSON packets without adding new message types or changing its binary ABI.

CEF's render process installs `Shared/JavaScriptBridge/BridgeScript.h` in the
main V8 context and forwards strings with `CefFrame::SendProcessMessage`. The
browser process puts events in a bounded queue; a nonblocking pipe worker sends
them without performing I/O on CEF's UI thread. UE accumulates partial headers
and payloads during `FVeloCEFConnection::Tick` and dispatches widget callbacks
on the game thread. Replacement connections discard callbacks from older hosts.

Ordinary `postMessage` strings use `JavaScriptMessage`. Internal messages start
with `__VELO_RPC__` followed by JSON with `version:1`, a document `session`, and
`kind` (`ready`, `request`, `result`, `cancel` or `unloaded`). Request/result IDs
correlate asynchronous calls. Native responses and events use quoted JSON in
`ExecuteJavaScript`; the page checks the session before dispatching. The header
ABI remains unchanged. `PageError` carries JSON `code`, `description`, `url`;
`ConsoleMessage` carries JSON `message`, `source`, `line`. Other navigation/title
events carry UTF-8 text.

`BrowserHost/tests/javascript_bridge_smoke.py` checks Unicode and console
messages, request success/rejection, Promise results/errors, timeout cancellation,
native-to-page string/JSON events, navigation sessions and JavaScript settings:

```powershell
python BrowserHost/tests/javascript_bridge_smoke.py --host BrowserHost/build/Release/VeloWebViewBrowserHost.exe
```

`VeloWebView.LayerTest` additionally sends an unhandled page request through
`UVeloWebView`, checks the Promise rejection, and posts a completion marker back
to UE (`Layer test JavaScript bridge received` in the log).

Windows input payloads are now binary POD records for mouse move/button/wheel
and CEF key events. `BrowserHost/tests/input_smoke.py` starts an offscreen host,
sends mouse, right-click, double-click, text, Unicode, shortcut, wheel and
slider-drag input through the named pipe, then checks DOM markers in the shared
BGRA ring. It is a transport/DOM regression test and does not inject OS input.

`BrowserHost/tests/select_popup_smoke.py` serves `Content/Test/SelectPopup.html`
over loopback HTTP and opens it with the `LoadUrl` command. It checks actual
option pixels in the shared frame, mouse and keyboard selection, popup wheel
scrolling, main-view repaints, Escape/outside-click dismissal, viewport edges,
resize and navigation. Popup pixels are composited in BrowserHost without
changing protocol version 1. Run after building the host:

```powershell
python BrowserHost/tests/select_popup_smoke.py --host BrowserHost/build/Release/VeloWebViewBrowserHost.exe --artifacts Saved/SelectPopupRegression/after
python BrowserHost/tests/input_smoke.py --host BrowserHost/build/Release/VeloWebViewBrowserHost.exe
```

For a manual Unreal check, set `UVeloWebView::RenderMode` to `Composited` and
call `LoadURL` with the file URL of `Content/Test/SelectPopup.html`. Open both
select controls, select Green, reopen and dismiss with Escape, then resize the
viewport and repeat. Rebuild and stage `VeloWebViewBrowserHost.exe` into
`Binaries/Win64` after host source changes, since that runtime bundle has
priority over `BrowserHost/build/Release` in the plugin's search order.
