# Build

## Baseline runtime

The repository targets UE 5.4, Windows x64, Visual Studio 2022 and Windows
SDK 10.0.19041 or newer. Download the pinned official CEF 154 standard package
without committing it:

```powershell
.\Scripts\SetupCEF.ps1
```

Build the separate host from a VS2022 x64 developer prompt. CEF standard
distributions include wrapper sources; a prebuilt wrapper can instead be passed
with `CEF_WRAPPER_LIBRARY`.

```powershell
cmake -S BrowserHost -B BrowserHost/build -G "Visual Studio 17 2022" -A x64 `
  -DCEF_ROOT="$PWD/ThirdParty/CEF"
cmake --build BrowserHost/build --config Release
```

The staged host directory must contain `VeloWebViewBrowserHost.exe`,
`libcef.dll`, `chrome_elf.dll`, `icudtl.dat`, the `.pak` files and `locales`.
The Unreal process never links or loads `libcef.dll`.

## Unreal plugin

```powershell
& "F:\Unreal\Engine\UE_5.4\Engine\Build\BatchFiles\Build.bat" `
  EmptyProjectEditor Win64 Development `
  -Project="$PWD\..\..\EmptyProject.uproject" -WaitMutex
```

The current Win64 build was verified with VS2022 and the plugin's external
connection code. Android keeps its existing engine WebBrowser path.

## H.264/WebRTC Pixel Streaming

The current `ThirdParty/CEF` has been replaced with the standalone custom CEF
154 distribution. Its `CEF_VERSION.json` records the H.264/AAC build flags and
`Release/libcef.dll` hash. For a future runtime upgrade, build a matching CEF/
Chromium distribution with the required codec flags:

```powershell
# Use an independent build root and the pinned CEF 8037-branch commit.
.\Scripts\BuildCEFWithCodecs.ps1 -CefProjectRoot F:\CEFBuild\cef-source `
  -BuildRoot F:\CEFBuild\work -OutputDir F:\CEFBuild\artifacts\CEF-H264
```

Review codec patent/licensing obligations, record the exact Chromium commit,
CEF version, GN args and checksums in `Docs/CEF_VERSION.md`, and validate H.264
in the actual WebRTC SDP. `Allow Media Capture`/`--allow-media-capture` only
controls getUserMedia permission; it does not add codec support.

The complete Chinese build, MP4/WebRTC acceptance and replacement procedure is
in `Docs/H264_BUILD.md`. Replace the entire matching distribution and rebuild
the ordinary BrowserHost. Codec support needs no extra widget switch.
