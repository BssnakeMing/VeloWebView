[CmdletBinding()]
param(
    [string]$Version = '154.0.32+g682c378+chromium-154.0.8037.58',
    # Do not use $PSScriptRoot in a parameter default. PowerShell evaluates
    # parameter defaults before the script scope has been initialized when the
    # script is launched with -File, which makes the default path empty on
    # some PowerShell 7 builds.
    [string]$Destination,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$scriptRoot = (Resolve-Path $PSScriptRoot).Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $repoRoot 'ThirdParty\CEF'
}
$archiveName = "cef_binary_${Version}_windows64.tar.bz2"
$downloadUrl = "https://cef-builds.spotifycdn.com/$archiveName"
# SHA-1 is the checksum published by cef-builds for this archive.  We also
# calculate SHA-256 and persist it in the manifest used by build/release jobs.
$publishedSha1 = '3ded4321d712c60fed298e752f0434b7bd3de43b'
$publishedSha256 = '767d475e25605d995b21ddb0b77de8f220fd370790d86198a2d2b82fdafdfbf9'
$root = $repoRoot
$cacheDir = Join-Path $root 'Intermediate\CEF'
$archivePath = Join-Path $cacheDir $archiveName
$extractDir = Join-Path $cacheDir 'extract'
$destinationPath = [IO.Path]::GetFullPath($Destination)

New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
if ($Force -or -not (Test-Path $archivePath)) {
    Write-Host "Downloading $downloadUrl"
    Invoke-WebRequest -UseBasicParsing -Uri $downloadUrl -OutFile $archivePath
}

$sha1 = (Get-FileHash -Algorithm SHA1 -LiteralPath $archivePath).Hash.ToLowerInvariant()
if ($sha1 -ne $publishedSha1) {
    throw "CEF archive SHA-1 mismatch. Expected $publishedSha1, got $sha1. Delete $archivePath and retry."
}
$sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
if ($sha256 -ne $publishedSha256) {
    throw "CEF archive SHA-256 mismatch. Expected $publishedSha256, got $sha256."
}

if (Test-Path $extractDir) { Remove-Item -LiteralPath $extractDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $extractDir | Out-Null
Write-Host 'Extracting CEF standard distribution...'
tar.exe -xjf $archivePath -C $extractDir
$top = Get-ChildItem -LiteralPath $extractDir -Directory | Select-Object -First 1
if (-not $top) { throw 'CEF archive did not contain a top-level directory.' }

if (Test-Path $destinationPath) {
    if (-not $Force) {
        throw "$destinationPath already exists. Re-run with -Force to replace it."
    }
    Remove-Item -LiteralPath $destinationPath -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $destinationPath | Out-Null
Get-ChildItem -LiteralPath $top.FullName | Copy-Item -Destination $destinationPath -Recurse -Force

$manifest = [ordered]@{
    cef_version = $Version
    chromium_version = '154.0.8037.58'
    distribution = 'windows64_standard'
    source = $downloadUrl
    published_sha1 = $publishedSha1
    published_sha256 = $publishedSha256
    sha256 = $sha256
    downloaded_utc = (Get-Date).ToUniversalTime().ToString('o')
    shared_texture_api = 'CefWindowInfo.shared_texture_enabled + CefRenderHandler::OnAcceleratedPaint'
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destinationPath 'CEF_VERSION.json') -Encoding UTF8
Remove-Item -LiteralPath $extractDir -Recurse -Force
Write-Host "CEF $Version installed in $destinationPath"
Write-Host "SHA-256: $sha256"
