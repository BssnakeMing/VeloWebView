# CEF runtime download

The CEF 154 runtime is intentionally distributed separately from the source
repository because it contains large third-party binaries and codec-enabled
build output.

Download the prepared Windows x64 archive here:

- [CEF_154.0.32_g682c378_chromium-154.0.8037.58_windows64_UE_WebBrowser.zip](https://1823780428.share.123pan.cn/123pan/MRgYjv-vIFOH?pwd=9lfo)
- 123 云盘提取码: `9lfo`

For a source checkout, extract the archive so the CEF distribution is available
at:

```text
VeloWebView/
  ThirdParty/
    CEF/
      Release/
      Resources/
      include/
      libcef_dll/
```

`Scripts/SetupCEF.ps1` remains available for users who prefer downloading the
pinned standard CEF package directly. The custom codec-enabled build is
documented in [`CEF_VERSION.md`](CEF_VERSION.md) and may require separate
patent and third-party license review before redistribution.

The GitHub Release asset `VeloWebView-1.0.0-Win64.zip` is a packaged plugin
from the tested checkout. It excludes Unreal's `Intermediate` build cache and
contains the compiled Win64 plugin and BrowserHost runtime.
