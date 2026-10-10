# Third-party notices

## Chromium Embedded Framework

Source: <https://github.com/chromiumembedded/cef>
License: BSD 3-Clause (see the CEF distribution's `LICENSE.txt`). The runtime
is downloaded by `Scripts/SetupCEF.ps1` and is not committed to this repository.

## Chromium

CEF bundles Chromium and its transitive dependencies. Their license and notice
files are shipped in the downloaded CEF distribution (`LICENSE.txt` and
`LICENSES.chromium.html`) and must remain beside the packaged runtime.

The implementation does not copy source from `ArtemIyX/CefHost`,
`obsproject/obs-browser`, `CefView/UCefView`, `CefView/CefViewCore`,
`starTechnology1994/uewebbrowser`, `getnamo/BLUI-Unreal`,
`mediabuff/cef-mixer`, or `fg-uulm/cef-spout`; those repositories were reviewed
as architectural references only. Their individual licenses should be reviewed
before importing any code.

The reference-project links and license decisions are tracked in
`Docs/REFERENCE_PROJECTS.md`.

The current `ThirdParty/CEF` runtime is a custom Chromium/CEF build with
`proprietary_codecs=true`, `ffmpeg_branding="Chrome"` and `rtc_use_h264=true`.
Its technical H.264/AAC validation is recorded in `Docs/CEF_VERSION.md` and the
standalone project report. Patent, FFmpeg/OpenH264 and other codec distribution
obligations remain a release gate; technical playback success is not a license.
