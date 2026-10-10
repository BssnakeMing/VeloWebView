# CEF runtime

The current Windows x64 runtime is the custom CEF standard distribution built
from the pinned CEF/Chromium source checkout in `F:\Project\CEFStandalone`:

| Field | Value |
| --- | --- |
| CEF | `154.0.32+g682c378+chromium-154.0.8037.58` |
| Chromium | `154.0.8037.58` |
| CEF commit | `682c378d70d5780061e96644dca16ddd8fd157a9` |
| Chromium commit | `a654841425914cbb703a2931e07b70a83aedbafd` |
| Distribution SHA-256 (`Release/libcef.dll`) | `6efb5f2c5e5724124a49fe7cff8cfe88e0c9f62fb7eabfdfca7d119356d583d4` |
| Codec GN args | `proprietary_codecs=true`, `ffmpeg_branding="Chrome"`, `rtc_use_h264=true` |
| CEF source | <https://github.com/chromiumembedded/cef> |

`ThirdParty/CEF/CEF_VERSION.json` records the standalone build manifest. The
previous open-codec runtime was backed up outside the plugin at
`F:\Project\CEFStandalone\VeloWebView-CEF-open-codec-backup-20261010`.

The host uses `CefWindowInfo.shared_texture_enabled` and
`CefRenderHandler::OnAcceleratedPaint` when GPU mode is enabled. CEF documents
that the shared handle is valid only during the callback, so the host publishes
metadata and copies a CPU fallback frame into the versioned ring for consumers
that do not yet have a D3D12 bridge.

The replaced runtime was validated through the real BrowserHost: the WebRTC
capability page reported `video/H264`, and a real H.264/AAC MP4 reached the
`playing` state in the CPU offscreen-rendering path. The standalone report still
requires a full SDP/`framesDecoded` WebRTC loopback record before release.
