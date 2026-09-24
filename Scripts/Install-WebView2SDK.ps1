[CmdletBinding()]
param(
    [string]$Version = "1.0.2903.40"
)

$ErrorActionPreference = "Stop"
$pluginRoot = (Resolve-Path (Join-Path $PSScriptRoot ".." )).Path
$packagePath = Join-Path $env:TEMP ("Microsoft.Web.WebView2." + $Version + ".nupkg")
$extractPath = Join-Path $env:TEMP ("velowebview-webview2-" + $Version)
$thirdParty = Join-Path $pluginRoot "ThirdParty\WebView2"

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        return $null
    }

    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset -gt ($bytes.Length - 6)) {
        return $null
    }
    if ([Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4) -ne ("PE" + [char]0 + [char]0)) {
        return $null
    }

    return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

Write-Host "Downloading the official Microsoft.Web.WebView2 $Version package..."
Invoke-WebRequest -Uri ("https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/" + $Version) -OutFile $packagePath
New-Item -ItemType Directory -Force -Path $extractPath | Out-Null
Expand-Archive -LiteralPath $packagePath -DestinationPath $extractPath -Force

$header = Get-ChildItem -LiteralPath $extractPath -Recurse -Filter WebView2.h | Select-Object -First 1
$loader = Get-ChildItem -LiteralPath $extractPath -Recurse -Filter WebView2Loader.dll |
    Where-Object {
        $_.FullName -match "\\runtimes\\win-x64\\native\\WebView2Loader\.dll$" -or
        $_.FullName -match "\\build\\native\\x64\\WebView2Loader\.dll$"
    } |
    Sort-Object { if ($_.FullName -match "\\runtimes\\win-x64\\native\\") { 0 } else { 1 } } |
    Select-Object -First 1
$library = Get-ChildItem -LiteralPath $extractPath -Recurse -Filter WebView2Loader.lib |
    Where-Object {
        $_.FullName -match "\\runtimes\\win-x64\\native\\WebView2Loader\.lib$" -or
        $_.FullName -match "\\build\\native\\x64\\WebView2Loader\.lib$"
    } |
    Select-Object -First 1
if (-not $header -or -not $loader) {
    throw "The official package did not contain WebView2.h and an x64 WebView2Loader.dll. Inspect $extractPath."
}

$loaderMachine = Get-PeMachine -Path $loader.FullName
if ($loaderMachine -ne 0x8664) {
    throw ("The selected WebView2Loader.dll is not an x64 PE image (machine 0x{0:X4}). Refusing to stage it." -f $loaderMachine)
}

New-Item -ItemType Directory -Force -Path (Join-Path $thirdParty "include"), (Join-Path $thirdParty "bin\x64"), (Join-Path $thirdParty "lib\x64") | Out-Null
Copy-Item -LiteralPath $header.FullName -Destination (Join-Path $thirdParty "include\WebView2.h") -Force
Copy-Item -LiteralPath $loader.FullName -Destination (Join-Path $thirdParty "bin\x64\WebView2Loader.dll") -Force
if ($library) { Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $thirdParty "lib\x64\WebView2Loader.lib") -Force }

Write-Host "Staged WebView2 SDK under $thirdParty"
Write-Host "Regenerate project files and rebuild. Install the Evergreen Runtime separately on target machines."
