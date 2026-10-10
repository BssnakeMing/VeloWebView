# Reference projects reviewed

These repositories were checked on 2026-09-30. They are references, not
vendored dependencies.

| Project | License | What is useful here | Decision |
| --- | --- | --- | --- |
| [chromiumembedded/cef](https://github.com/chromiumembedded/cef) | BSD-3-Clause | Official API and Windows shared-texture contract | Runtime source |
| [chromiumembedded/cef-project](https://github.com/chromiumembedded/cef-project) | BSD-style CEF | CMake/VS2022 bootstrap and CEF download flow | Build reference |
| [ArtemIyX/CefHost](https://github.com/ArtemIyX/CefHost) | MIT | D3D11 `OnAcceleratedPaint`, ring, shared memory and handles | Closest host reference; its fixed global names were deliberately avoided |
| [ArtemIyX/WebUserInterfaceUnreal](https://github.com/ArtemIyX/WebUserInterfaceUnreal) | MIT | UE consumer and Slate surface split | UE5.7-oriented; not copied into UE5.4 |
| [obsproject/obs-browser](https://github.com/obsproject/obs-browser) | GPL-2.0 | GPU OSR, input and browser lifecycle | Architecture only; GPL code is not imported |
| [CefView/UCefView](https://github.com/CefView/UCefView) | BSD-3-Clause | UE integration and CEF packaging ideas | API comparison only |
| [getnamo/BLUI-Unreal](https://github.com/getnamo/BLUI-Unreal) | MIT | External CEF staging and Slate/material integration | Older UE target; no source copied |
| [mediabuff/cef-mixer](https://github.com/mediabuff/cef-mixer) | Check repository | Texture/mixer experiments | Future GPU reference |
| [fg-uulm/cef-spout](https://github.com/fg-uulm/cef-spout) | Check repository | Windows texture sharing | Future interop reference |

`starTechnology1994/uewebbrowser`, `CefView/CefViewCore` and
`surgeshi/FFMPEGMediaUE5` were also considered. RTSP/FFmpeg is intentionally
not part of this browser MVP.
