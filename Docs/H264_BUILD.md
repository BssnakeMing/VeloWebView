# CEF H.264/AAC 构建、验收与回到 VeloWebView 的部署流程

本文供另一个独立构建项目直接执行。目标是得到与当前 VeloWebView 完全匹配的
Windows x64 CEF standard distribution，同时支持 HTML5 MP4 的 H.264/AAC 和
WebRTC/UE Pixel Streaming 的 H.264。本文和脚本已按锁定版本的上游源码核对；
本项目尚未编译此自定义 CEF，也尚未验证其实际视频播放。

## 1. 必须锁定的版本

| 项目 | 值 |
| --- | --- |
| CEF 完整版本 | `154.0.32+g682c378+chromium-154.0.8037.58` |
| CEF Git commit | `682c378d70d5780061e96644dca16ddd8fd157a9` |
| CEF Git 分支 / automate-git 的 branch | **`8037`** |
| Chromium tag | `refs/tags/154.0.8037.58` |
| 平台 | Windows x64，Release，非 component build |
| 分发包 | standard，保留 C++ wrapper 源码、头文件和 `libcef.lib` |

CEF 的主版本是 154，但 Git 分支号是 Chromium 的 build 号 **8037**。
不要使用 `--branch=154`，也不要使用 `master` 或 8037 分支的最新 commit。
上述 commit 的 `CHROMIUM_BUILD_COMPATIBILITY.txt` 指定了对应 Chromium tag；
让 CEF automate-git 自动选择它，不额外覆盖 Chromium revision。

当前本项目插件目录为：

```text
F:\Unreal\UnrealProject\5.4cc\EmptyProject\Plugins\VeloWebView
```

当前官方二进制不提供所需 codec。H.264 支持取决于重新编译后的 Chromium/CEF，
没有 UE 属性或 `--enable-h264` 参数能把 decoder 加到现有 DLL 中。

## 2. 独立构建项目的工作范围

独立项目负责下载源码、编译、生成完整 standard 包、运行两种视频验收并交付
包和报告。回到本项目后再替换 `ThirdParty/CEF`、重建 BrowserHost 和 UE 插件。
本阶段不向当前插件写入自定义 DLL。

建议使用本地 SSD 短路径，例如 `F:\CEFBuild`，不要带空格或中文。预留至少
500 GB 可用空间，建议 32 GB 以上内存，64 GB 更适合完整 Chromium 构建。
需要稳定下载 Chromium/depot_tools/CIPD 的网络和足够长的构建时间。

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”、Windows SDK、Git、Python 3，
并按锁定 Chromium 版本的 Windows build instructions 检查工具链要求。CEF
内部使用 Chromium 固定的 clang 工具链，VS 主要提供 SDK/CRT。不要强制
`is_clang=false`。CMake 用于随后编译分发包里的 cefclient 和本项目 BrowserHost。

## 3. 准备源码和脚本

在 PowerShell 中执行，示例根目录为 `F:\CEFBuild`：

```powershell
$cefTaskRoot = 'F:\CEFBuild'
New-Item -ItemType Directory -Path $cefTaskRoot -Force | Out-Null
git clone https://github.com/chromiumembedded/cef.git "$cefTaskRoot\cef-source"
git -C "$cefTaskRoot\cef-source" checkout 682c378d70d5780061e96644dca16ddd8fd157a9
git -C "$cefTaskRoot\cef-source" rev-parse HEAD
Get-Content "$cefTaskRoot\cef-source\CHROMIUM_BUILD_COMPATIBILITY.txt"

$veloPlugin = 'F:\Unreal\UnrealProject\5.4cc\EmptyProject\Plugins\VeloWebView'
Copy-Item -LiteralPath "$veloPlugin\Scripts\BuildCEFWithCodecs.ps1" `
  -Destination "$cefTaskRoot\BuildCEFWithCodecs.ps1"
Copy-Item -LiteralPath "$veloPlugin\Content\CodecTest.html" `
  -Destination "$cefTaskRoot\CodecTest.html"
```

该脚本默认把 Chromium/depot_tools 下载在独立的 `work` 目录，生成完整分发包，
检查版本和有效 GN 参数，最后复制到空的 `artifacts\CEF-H264`。它不会清空已有
源码，也不会把“成功编译”记作“成功播放”。

先检查将要执行的参数：

```powershell
& "$cefTaskRoot\BuildCEFWithCodecs.ps1" `
  -CefProjectRoot "$cefTaskRoot\cef-source" `
  -BuildRoot "$cefTaskRoot\work" `
  -OutputDir "$cefTaskRoot\artifacts\CEF-H264" `
  -PrintCommandOnly
```

然后去掉 `-PrintCommandOnly` 执行构建：

```powershell
& "$cefTaskRoot\BuildCEFWithCodecs.ps1" `
  -CefProjectRoot "$cefTaskRoot\cef-source" `
  -BuildRoot "$cefTaskRoot\work" `
  -OutputDir "$cefTaskRoot\artifacts\CEF-H264"
```

构建失败后可在同一 `work` 目录继续执行。脚本使用 `--force-build` /
`--force-distrib` 强制重新检查构建和生成分发包，但不使用会删除源码的
`--force-clean`。如果产物输出目录已有文件，改用新的空输出目录，避免混包。

## 4. 编译参数与有效性检查

脚本在 CEF 的 GN hook **之前**设置：

```text
GN_DEFINES=is_official_build=true is_component_build=false proprietary_codecs=true ffmpeg_branding="Chrome" rtc_use_h264=true symbol_level=1
DEPOT_TOOLS_WIN_TOOLCHAIN=0
```

核心 codec 参数：

- `proprietary_codecs=true`：启用 Chromium 对 H.264/AAC 等格式的支持。
- `ffmpeg_branding="Chrome"`：选择包含相应 decoder 的 FFmpeg 配置。
- `rtc_use_h264=true`：保留 WebRTC H.264 编解码路径。

锁定的 WebRTC 源码中，Chromium 集成构建的 `rtc_use_h264` 默认由
`media_use_openh264` 推导；`proprietary_codecs=true` 时 Windows x64 默认开启
该路径。仍需检查最终 GN 值及实际能力，不能只检查环境变量。
不要沿用旧脚本的 `rtc_include_ilbc=false` 或先构建 open-codec 包后再手工
`gn gen` 到另一个目录；这可能忽略配置或导致拿到错误分发包。

有效输出目录和日志：

```text
F:\CEFBuild\work\chromium\src\out\Release_GN_x64\args.gn
F:\CEFBuild\work\build-8037-release.log
F:\CEFBuild\work\chromium\src\cef\binary_distrib\cef_binary_154.0.32+g682c378+chromium-154.0.8037.58_windows64
```

在构建环境中还可检查 GN 展开后的参数（找到实际 `gn.exe` 后执行）：

```powershell
Set-Location "$cefTaskRoot\work\chromium\src"
gn args out/Release_GN_x64 --list=proprietary_codecs
gn args out/Release_GN_x64 --list=ffmpeg_branding
gn args out/Release_GN_x64 --list=rtc_use_h264
gn args out/Release_GN_x64 --list=media_use_openh264
```

执行前根据输出确认程序与路径正确。若 GN 报“Build argument has no effect”，
查锁定 revision 的 `.gni`，不要忽略后直接宣称支持 H.264。

## 5. 产物和播放验收

`artifacts\CEF-H264` 至少交付：

```text
include/             # cef_app.h、cef_version.h、API hash/version headers
libcef_dll/          # wrapper 源码
cmake/
Release/            # libcef.dll、libcef.lib、chrome_elf.dll、GPU helper DLL 等
Resources/          # icudtl.dat、*.pak、locales/
CMakeLists.txt
LICENSE.txt
CEF_VERSION.json    # 脚本记录 exact commits、GN_DEFINES、libcef SHA256
BUILD_ARGS.gn       # 有效 args.gn
VALIDATION.md       # 由独立项目补充验收结果
```

从这个分发包新建 CMake build 目录编译 cefclient；不要复用其他 CEF 的 wrapper：

```powershell
$cefArtifact = "$cefTaskRoot\artifacts\CEF-H264"
cmake -S $cefArtifact -B "$cefTaskRoot\cefclient-build" `
  -G 'Visual Studio 17 2022' -A x64
cmake --build "$cefTaskRoot\cefclient-build" --config Release --target cefclient
```

验收可使用本项目提供的 `CodecTest.html`，通过本地 HTTP 提供该页及测试视频：

```powershell
# 把 CodecTest.html、H.264+AAC 的 sample.mp4 放在 F:\CEFBuild\test-pages
python -m http.server 8765 --bind 127.0.0.1 --directory "$cefTaskRoot\test-pages"
# 在另一个终端启动刚编译的 cefclient，按实际路径定位 EXE
& "$cefTaskRoot\cefclient-build\tests\cefclient\Release\cefclient.exe" `
  --url=http://127.0.0.1:8765/CodecTest.html
```

没有样本时，可用已安装 FFmpeg 生成几秒 H.264/AAC 视频：

```powershell
ffmpeg -f lavfi -i 'testsrc2=size=640x360:rate=30' `
  -f lavfi -i 'sine=frequency=440:sample_rate=48000' -t 6 `
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline `
  -c:a aac -b:a 128k -movflags +faststart "$cefTaskRoot\test-pages\sample.mp4"
```

验收要求：

1. HTML5：`canPlayType` / MediaSource codec 检查通过；sample.mp4 真正出现运动
   画面，时间持续增长，解除静音后能听到 AAC 音频，没有 media error。
2. WebRTC：sender 和 receiver capability 含 `video/H264`；测试页只允许 H.264
   创建本地 peer connection，实际 SDP 包含 H264，入站 `framesDecoded` 增长。
   使用真实 UE Pixel Streaming 服务再测一次，确认接收 codec 和动态画面。
3. 在与当前插件一致的 CPU OSR 条件下（`--off-screen-rendering-enabled`
   配合 `--disable-gpu`）复测；硬件 codec 的可用性和普通 cefclient 窗口不等于
   当前 BrowserHost 的渲染路径。

没有 sample.mp4 时，测试页只能报告能力和 WebRTC loopback，不能证明 MP4/AAC
实际播放。自定义 codec 支持也不包含 Widevine/DRM 授权、HEVC 或服务器 CORS
配置；对外分发前记录 H.264/AAC、FFmpeg、OpenH264 的许可处理结果。

## 6. 构建完成后回到当前插件

确认验收报告通过后，在此项目关闭 UE 编辑器及该项目的 BrowserHost。备份
当前 `ThirdParty/CEF`，再用**整个 standard 包**替换；不要只复制 libcef.dll
或把其他版本的 FFmpeg DLL 放进去。

示例 PowerShell（此步骤由回到本项目的任务执行）：

```powershell
$veloPlugin = 'F:\Unreal\UnrealProject\5.4cc\EmptyProject\Plugins\VeloWebView'
$cefArtifact = 'F:\CEFBuild\artifacts\CEF-H264'
Set-Location $veloPlugin
$cefBackup = Join-Path $veloPlugin ('ThirdParty\CEF-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
Move-Item -LiteralPath "$veloPlugin\ThirdParty\CEF" -Destination $cefBackup
Copy-Item -LiteralPath $cefArtifact -Destination "$veloPlugin\ThirdParty\CEF" -Recurse
```

必须重建 BrowserHost。使用新的 CMake 目录，避免旧缓存中的 `CEF_LIBRARY` 或
`CEF_WRAPPER_LIBRARY` 指向 open-codec 包。按实际 VS 路径定位 cmake，注意该机器
`VCTargetsPath` 若存在必须保留末尾反斜杠：

```powershell
$env:VCTargetsPath = 'F:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\'
$cmakeExe = 'F:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmakeExe -S BrowserHost -B BrowserHost/build-h264-replacement `
  -G 'Visual Studio 17 2022' -A x64 `
  "-DCEF_ROOT=$veloPlugin\ThirdParty\CEF"
& $cmakeExe --build BrowserHost/build-h264-replacement --config Release `
  --target VeloWebViewBrowserHost
```

检查 `BrowserHost/build-h264-replacement/Release` 的所有 runtime 文件与新包
一致。它已经包含 EXE、DLL、*.pak、locales，必须整体部署。当前默认查找顺序
还包括旧的 `BrowserHost/build/Release`，所以回到本项目时将旧的完整目录备份，
把新目录整体复制到默认位置：

```powershell
if (Test-Path -LiteralPath "$veloPlugin\BrowserHost\build\Release") {
  $hostBackup = Join-Path $veloPlugin ('BrowserHost\build\Release-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
  Move-Item -LiteralPath "$veloPlugin\BrowserHost\build\Release" -Destination $hostBackup
}
Copy-Item -LiteralPath "$veloPlugin\BrowserHost\build-h264-replacement\Release" `
  -Destination "$veloPlugin\BrowserHost\build\Release" -Recurse

& 'F:\Unreal\Engine\UE_5.4\Engine\Build\BatchFiles\Build.bat' `
  EmptyProjectEditor Win64 Development `
  '-Project=F:\Unreal\UnrealProject\5.4cc\EmptyProject\EmptyProject.uproject' `
  -WaitMutex -NoHotReloadFromIDE
```

继续使用 `RenderMode=Composited` 和普通 `VeloWebViewBrowserHost.exe`。
不需要 `bEnableH264` 属性、特殊命名的 EXE 或伪造“支持 H.264”的 marker。
能力探测、实际 MP4 和实际 WebRTC 播放是验收依据。`bEnablePixelStreaming`
可调节 autoplay/background throttling，`bAllowMediaCapture` 仅按业务需要启用。

如果项目 `Binaries/Win64` 或引擎 EXE 目录有更高优先级的旧 BrowserHost bundle，
同时更新或备份它，并在实际子进程命令行中确认启动的是新目录。最后用
Composited 控件打开测试页和真实业务页，确认两种视频播放、鼠标键盘交互、
反复启停、多实例和打包后的运行时文件。

## 7. 独立项目交接报告模板

```text
CEF_VERSION / CEF_COMMIT:
CHROMIUM_TAG / CHROMIUM_COMMIT:
构建目录 / 完整产物目录:
有效 GN 参数 / libcef.dll SHA256:
HTML5 MP4 H.264+AАC：运动画面、播放时间和音频测试结果:
WebRTC H.264：capability、SDP、framesDecoded、Pixel Streaming 测试结果:
CPU OSR / disable-gpu 条件的结果:
工具链 / SDK / GPU 驱动:
许可处理记录:
遗留问题、截图和日志路径:
```

交接给本项目时提供完整包所在的绝对路径及 `VALIDATION.md`，不要只提供 DLL。

## 8. 依据

- [锁定 CEF commit](https://github.com/chromiumembedded/cef/tree/682c378d70d5780061e96644dca16ddd8fd157a9)
- [Chromium 兼容版本](https://github.com/chromiumembedded/cef/blob/682c378d70d5780061e96644dca16ddd8fd157a9/CHROMIUM_BUILD_COMPATIBILITY.txt)
- [对应 automate-git.py](https://github.com/chromiumembedded/cef/blob/682c378d70d5780061e96644dca16ddd8fd157a9/tools/automate/automate-git.py)
- [对应 Chromium Windows 构建要求](https://chromium.googlesource.com/chromium/src/+/refs/tags/154.0.8037.58/docs/windows_build_instructions.md)
- [对应 WebRTC H.264 参数定义](https://webrtc.googlesource.com/src/+/3a8b0c76e6b7bf0e72141047f82eb2c3f4862c8d/webrtc.gni)
