[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CefProjectRoot,
    [string]$BuildRoot,
    [string]$OutputDir,
    [switch]$PrintCommandOnly
)

$ErrorActionPreference = 'Stop'
$cefCommit = '682c378d70d5780061e96644dca16ddd8fd157a9'
$cefBranch = '8037'
$cefVersion = '154.0.32+g682c378+chromium-154.0.8037.58'
$cefSourcePath = (Resolve-Path -LiteralPath $CefProjectRoot).Path
$automate = Join-Path $cefSourcePath 'tools\automate\automate-git.py'
if (-not (Test-Path -LiteralPath $automate -PathType Leaf)) {
    throw 'CefProjectRoot must be a chromiumembedded/cef source checkout containing tools\automate\automate-git.py.'
}
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path (Split-Path $cefSourcePath -Parent) 'work'
}
$cefBuildPath = [IO.Path]::GetFullPath($BuildRoot)
if ($cefBuildPath -match '\s') { throw 'CEF automate-git requires a download/build path without spaces.' }
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path (Split-Path $cefSourcePath -Parent) 'artifacts\CEF-H264'
}
$cefOutputPath = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $cefOutputPath) {
    if (Get-ChildItem -LiteralPath $cefOutputPath -Force | Select-Object -First 1) {
        throw "OutputDir must be empty to avoid mixing CEF distributions: $cefOutputPath"
    }
}

# CEF uses Chromium's branch number (8037), not the CEF major (154).
# Set codec arguments BEFORE automate-git runs CEF's GN hook and build.
# Chromium uses its pinned clang toolchain; VS supplies the CRT/SDK.
$gnDefines = 'is_official_build=true is_component_build=false proprietary_codecs=true ffmpeg_branding="Chrome" rtc_use_h264=true symbol_level=1'
$arguments = @(
    "--download-dir=$cefBuildPath",
    "--depot-tools-dir=$(Join-Path $cefBuildPath 'depot_tools')",
    "--branch=$cefBranch",
    "--checkout=$cefCommit",
    '--x64-build', '--no-debug-build', '--build-target=cefclient cefsimple',
    '--force-build', '--force-distrib', '--no-distrib-symbols',
    '--no-distrib-docs', '--no-distrib-archive', '--build-log-file'
)
Write-Output "GN_DEFINES=$gnDefines"
Write-Output "python $automate"
$arguments | ForEach-Object { Write-Output "  $_" }
Write-Output "Distribution destination: $cefOutputPath"
if ($PrintCommandOnly) { return }

$oldGnDefines = $env:GN_DEFINES
$oldToolchain = $env:DEPOT_TOOLS_WIN_TOOLCHAIN
$oldPath = $env:PATH
try {
    $env:GN_DEFINES = $gnDefines
    $env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
    $env:PATH = (Join-Path $cefBuildPath 'depot_tools') + ';' + $env:PATH
    & python $automate @arguments
    if ($LASTEXITCODE -ne 0) { throw "CEF automation failed with exit code $LASTEXITCODE. See $cefBuildPath\build-$cefBranch-release.log." }

    $src = Join-Path $cefBuildPath 'chromium\src'
    $argsFile = Join-Path $src 'out\Release_GN_x64\args.gn'
    $effectiveArgs = Get-Content -LiteralPath $argsFile -Raw
    foreach ($pattern in @('proprietary_codecs\s*=\s*true', 'ffmpeg_branding\s*=\s*"Chrome"', 'rtc_use_h264\s*=\s*true')) {
        if ($effectiveArgs -notmatch $pattern) { throw "Required codec argument missing in $argsFile : $pattern" }
    }
    $distribRoot = Join-Path $src 'cef\binary_distrib'
    $distributions = @(Get-ChildItem -LiteralPath $distribRoot -Directory |
        Where-Object { $_.Name -eq "cef_binary_${cefVersion}_windows64" })
    if ($distributions.Count -ne 1) { throw "Expected exactly one matching standard distribution under $distribRoot." }
    $distribution = $distributions[0].FullName
    foreach ($relative in @('include\cef_version.h', 'libcef_dll\CMakeLists.txt', 'Release\libcef.dll', 'Release\libcef.lib', 'Resources\resources.pak')) {
        if (-not (Test-Path -LiteralPath (Join-Path $distribution $relative) -PathType Leaf)) { throw "Incomplete standard distribution: $relative" }
    }
    $versionHeader = Get-Content -LiteralPath (Join-Path $distribution 'include\cef_version.h') -Raw
    if ($versionHeader -notmatch [regex]::Escape($cefVersion) -or $versionHeader -notmatch $cefCommit) {
        throw 'Generated CEF version/commit differs from the pinned plugin headers.'
    }
    New-Item -ItemType Directory -Path $cefOutputPath -Force | Out-Null
    Get-ChildItem -LiteralPath $distribution -Force | Copy-Item -Destination $cefOutputPath -Recurse
    $cefOutputDll = Join-Path $cefOutputPath 'Release\libcef.dll'
    $manifest = [ordered]@{
        cef_version = $cefVersion
        cef_commit = $cefCommit
        chromium_version = '154.0.8037.58'
        chromium_commit = ((& git -C $src rev-parse HEAD) -join '').Trim()
        cef_branch = $cefBranch
        distribution = 'windows64_standard_custom'
        gn_defines = $gnDefines
        libcef_sha256 = (Get-FileHash -LiteralPath $cefOutputDll -Algorithm SHA256).Hash.ToLowerInvariant()
        generated_utc = [DateTime]::UtcNow.ToString('o')
        verification_status = 'built; MP4/AAC playback and H264 WebRTC runtime tests still required'
    }
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $cefOutputPath 'CEF_VERSION.json') -Encoding UTF8
    Copy-Item -LiteralPath $argsFile -Destination (Join-Path $cefOutputPath 'BUILD_ARGS.gn')
    Write-Output "Built $cefOutputPath. Follow Docs/H264_BUILD.md for runtime acceptance and replacement; no playback support is certified by this script."
} finally {
    $env:GN_DEFINES = $oldGnDefines
    $env:DEPOT_TOOLS_WIN_TOOLCHAIN = $oldToolchain
    $env:PATH = $oldPath
}
