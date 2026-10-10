# 独立 CEF/Chromium Windows x64 构建与编译

本文是一份**独立的 CEF 构建文档**。它不依赖 Unreal、VeloWebView、BrowserHost
或任何 UE 工程，也不执行插件替换和插件打包。最终产物是一个独立的 CEF
standard distribution，以及用该 distribution 编译的 `cefclient`/`cefsimple`。

## 1. 固定版本

本次构建使用 CEF 154 对应的 Chromium 版本：

| 项目 | 固定值 |
| --- | --- |
| CEF | `154.0.32+g682c378+chromium-154.0.8037.58` |
| CEF commit | `682c378d70d5780061e96644dca16ddd8fd157a9` |
| CEF branch | `8037` |
| Chromium tag | `refs/tags/154.0.8037.58` |
| 平台 | Windows x64，Release，non-component |

CEF 的主版本是 154，但 `automate-git.py --branch` 使用 Chromium build branch
`8037`。不要使用 `--branch=154`、`master`，也不要使用 8037 分支的最新提交。

## 2. 构建机要求

- Windows 10/11 x64。
- Visual Studio 2022，安装“使用 C++ 的桌面开发”和 Windows SDK。
- Git、Python 3、PowerShell。
- `depot_tools` 由 CEF automation 下载或使用已有副本。
- `gn`、`autoninja`、Ninja 和 Chromium 所需的 CIPD 工具。
- 建议至少 32 GB 内存、500 GB 可用磁盘空间。
- 所有源码和 download/build 目录使用不带空格和非 ASCII 字符的短路径。

Codec 构建会启用 H.264/AAC 相关代码。发布前必须单独审核 FFmpeg、OpenH264
和 H.264/AAC 专利、源代码及分发许可；编译成功不代表拥有分发权。

## 3. 准备独立工作目录

以下示例完全位于 `F:\Project\CEFStandalone`，不会写入任何 UE 或插件目录：

```powershell
$root = 'F:\Project\CEFStandalone'
New-Item -ItemType Directory -Path $root -Force | Out-Null
git clone https://github.com/chromiumembedded/cef.git "$root\cef-source"
git -C "$root\cef-source" checkout 682c378d70d5780061e96644dca16ddd8fd157a9
git -C "$root\cef-source" rev-parse HEAD
Get-Content "$root\cef-source\CHROMIUM_BUILD_COMPATIBILITY.txt"
```

`rev-parse` 必须输出固定的 CEF commit，兼容文件必须指向
`refs/tags/154.0.8037.58`。如果已有源码 checkout，先确认它没有未提交修改。

## 4. Codec GN 配置

Chromium/CEF 构建使用 Chromium 自己的 clang 工具链；VS 负责 Windows SDK 和
运行库。必须在运行 `automate-git.py` 前设置 `GN_DEFINES`：

```powershell
$env:GN_DEFINES = 'is_official_build=true is_component_build=false proprietary_codecs=true ffmpeg_branding="Chrome" rtc_use_h264=true symbol_level=1'
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
```

这些参数的含义：

- `proprietary_codecs=true`：启用 Chromium 的专有媒体 codec 配置。
- `ffmpeg_branding="Chrome"`：选择包含 H.264/AAC 所需路径的 FFmpeg 配置。
- `rtc_use_h264=true`：保留 WebRTC H.264 路径。
- `is_component_build=false`：生成可分发的 non-component CEF。

不要在构建后只改 `args.gn` 再直接复制 DLL；必须重新生成受影响的 targets 和
standard distribution。

## 5. 下载源码、编译并生成 standard distribution

```powershell
$root = 'F:\Project\CEFStandalone'
$download = "$root\work"
$cef = "$root\cef-source"
$depot = "$download\depot_tools"
$automate = "$cef\tools\automate\automate-git.py"

New-Item -ItemType Directory -Path $download -Force | Out-Null
$env:PATH = "$depot;$env:PATH"

python $automate `
  --download-dir="$download" `
  --depot-tools-dir="$depot" `
  --branch=8037 `
  --checkout=682c378d70d5780061e96644dca16ddd8fd157a9 `
  --x64-build `
  --no-debug-build `
  --build-target="cefclient cefsimple" `
  --force-build `
  --force-distrib `
  --no-distrib-symbols `
  --no-distrib-docs `
  --no-distrib-archive `
  --build-log-file
```

成功后，standard distribution 通常位于：

```text
F:\Project\CEFStandalone\work\chromium\src\cef\binary_distrib\cef_binary_154.0.32+g682c378+chromium-154.0.8037.58_windows64
```

如果路径不同，按 `binary_distrib` 下的目录名和版本头文件定位；不要混用其他
CEF 版本的 `include`、`libcef.lib`、`Resources` 或 `libcef.dll`。

## 6. 验证编译参数和 distribution

先确认 GN 最终文件，而不是只确认环境变量：

```powershell
$argsFile = 'F:\Project\CEFStandalone\work\chromium\src\out\Release_GN_x64\args.gn'
Select-String -LiteralPath $argsFile -Pattern `
  'proprietary_codecs\s*=\s*true',
  'ffmpeg_branding\s*=\s*"Chrome"',
  'rtc_use_h264\s*=\s*true',
  'is_component_build\s*=\s*false',
  'target_cpu\s*=\s*"x64"'
```

检查 standard distribution 的关键文件：

```powershell
$dist = 'F:\Project\CEFStandalone\work\chromium\src\cef\binary_distrib\cef_binary_154.0.32+g682c378+chromium-154.0.8037.58_windows64'
@(
  'include\cef_version.h',
  'libcef_dll\CMakeLists.txt',
  'Release\libcef.dll',
  'Release\libcef.lib',
  'Resources\resources.pak'
) | ForEach-Object {
  $path = Join-Path $dist $_
  if (-not (Test-Path -LiteralPath $path)) { throw "Missing: $path" }
  Write-Output $path
}
Get-Content "$dist\include\cef_version.h" | Select-String 'CEF_VERSION|CEF_COMMIT_HASH|CHROME_VERSION'
Get-FileHash "$dist\Release\libcef.dll" -Algorithm SHA256
```

建立一个独立 manifest，记录 CEF/Chromium commit、完整 `args.gn`、生成时间、
`libcef.dll` SHA256 和许可审核结果。不要只记录 CEF 主版本号。

## 7. 用 distribution 编译独立 cefclient

CEF standard distribution 自带根 `CMakeLists.txt` 和 `tests/cefclient`。从 distribution
目录新建单独的 CMake build 目录：

```powershell
$dist = 'F:\Project\CEFStandalone\work\chromium\src\cef\binary_distrib\cef_binary_154.0.32+g682c378+chromium-154.0.8037.58_windows64'
$clientBuild = 'F:\Project\CEFStandalone\cefclient-build'
$env:VCTargetsPath = 'F:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\'
$cmake = 'F:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

& $cmake -S $dist -B $clientBuild -G 'Visual Studio 17 2022' -A x64
& $cmake --build $clientBuild --config Release --target cefclient cefsimple
```

运行目录必须保留完整 runtime：`cefclient.exe` 旁边需要 `libcef.dll`、
`chrome_elf.dll`、`icudtl.dat`、所有 `.pak`、`locales` 和分发包提供的其他 DLL。
不要只把 EXE 拷贝到桌面运行。

## 8. HTML5 H.264/AAC 验收

在独立目录准备测试页和视频：

```powershell
New-Item -ItemType Directory -Path F:\Project\CEFStandalone\test-pages -Force | Out-Null
@'
<!doctype html><meta charset="utf-8"><video id="v" controls muted playsinline src="sample.mp4"></video>
<pre id="s"></pre><script>
const v=document.querySelector('#v'),s=document.querySelector('#s');
function show(e){s.textContent=JSON.stringify({event:e,readyState:v.readyState,
 currentTime:v.currentTime,duration:v.duration,width:v.videoWidth,height:v.videoHeight,
 error:v.error&&{code:v.error.code,message:v.error.message}},null,2)}
['loadedmetadata','canplay','playing','timeupdate','error','ended'].forEach(e=>v.addEventListener(e,()=>show(e)));
show('initial');
</script>
'@ | Set-Content F:\Project\CEFStandalone\test-pages\index.html -Encoding UTF8

ffmpeg -f lavfi -i 'testsrc2=size=640x360:rate=30' `
  -f lavfi -i 'sine=frequency=440:sample_rate=48000' -t 6 `
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline `
  -c:a aac -b:a 128k -movflags +faststart `
  F:\Project\CEFStandalone\test-pages\sample.mp4
python -m http.server 8765 --bind 127.0.0.1 --directory F:\Project\CEFStandalone\test-pages
```

在另一个终端，从独立 cefclient 的目录打开：

```powershell
& 'F:\Project\CEFStandalone\cefclient-build\tests\cefclient\Release\cefclient.exe' `
  --url=http://127.0.0.1:8765/index.html
```

验收必须同时满足：`canplay` 触发、视频尺寸非零、`currentTime` 持续增加、
画面有运动、解除静音后能听到 AAC。`canPlayType` 返回非空只能作为预检，不能
替代实际 MP4 播放。

## 9. WebRTC H.264 验收

在独立测试页先打印能力：

```javascript
const sender = RTCRtpSender.getCapabilities?.('video')?.codecs ?? [];
const receiver = RTCRtpReceiver.getCapabilities?.('video')?.codecs ?? [];
console.log({senderH264: sender.filter(c => c.mimeType?.toLowerCase() === 'video/h264'),
             receiverH264: receiver.filter(c => c.mimeType?.toLowerCase() === 'video/h264')});
```

再做 H.264-only 的本机 loopback：使用 `canvas.captureStream()` 创建视频轨，
通过 `RTCRtpTransceiver.setCodecPreferences()` 只保留 `video/H264`，建立 offer/
answer，并观察 receiver 的 `inbound-rtp.framesDecoded`、帧尺寸和 `codec.mimeType`。
如果 capability 没有 H.264，就不要把 VP8/VP9 的成功误判为 H.264 通过。

最后使用真实 UE Pixel Streaming/信令服务验证：检查最终 SDP 的 H.264 payload、
接收统计和画面。cefclient 的本机 loopback 通过不代表服务器、硬件编码器、
profile/level、TLS 或网络路径一定通过。

## 10. 常见问题

- **只看到 VP8/VP9/AV1**：检查最终 `args.gn`，确认 `proprietary_codecs`、
  `ffmpeg_branding`、`rtc_use_h264` 都是有效值；删除对应 `out` 目录后重新 GN
  和 build，不能只替换 DLL。
- **MP4 有画面但没有声音**：确认 AAC track、浏览器音频策略和 sample 的容器；
  重新生成 `-movflags +faststart` 的测试文件。
- **cefclient 启动崩溃/缺 DLL**：从 distribution 根 CMake 编译并整体运行 Release
  目录，检查所有 `.pak`、`locales`、`icudtl.dat` 和 helper DLL。
- **WebRTC 只协商 VP8**：检查 `setCodecPreferences` 是否只放入 H.264，检查最终
  SDP，不要只看 sender capability。
- **构建路径混用了旧包**：新建 CMake build 目录，检查 `CEF_ROOT`、`libcef.lib`
  和 `cef_version.h` 的绝对路径与 SHA256。

## 11. 独立交付清单

交付给其他工程时只交付独立产物和报告：

```text
cef_binary_..._windows64/       # 完整 standard distribution
cefclient-build/                # 可选，独立 cefclient/cefsimple 编译产物
CEF_VERSION.json                # CEF/Chromium commit、GN args、SHA256
BUILD_ARGS.gn                   # 最终有效 GN 参数
VALIDATION.md                   # MP4/AAC、H264 WebRTC、SDP、framesDecoded 结果
LICENSES/                       # codec 和第三方许可记录
```

此独立流程结束后，产物可以被其他 CEF 宿主项目消费；本文不规定任何 Unreal
插件目录、UE Build.cs、BrowserHost IPC 或项目打包方式。
