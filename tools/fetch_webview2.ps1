# Colaimdation - vendor the WebView2 SDK
#
# Downloads the Microsoft.Web.WebView2 NuGet package into third_party/webview2
# so the build has no external prerequisites beyond Visual Studio and CMake.
# Re-running refreshes to the newest stable SDK.

[CmdletBinding()]
param(
    [string]$Version = "",        # e.g. 1.0.3351.48; empty = newest stable
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $root "third_party"
$extract = Join-Path $thirdParty "webview2"

$header = Join-Path $extract "build/native/include/WebView2.h"
if ((Test-Path $header) -and -not $Force) {
    Write-Host "WebView2 SDK already present at $extract (use -Force to refresh)" -ForegroundColor DarkGray
    exit 0
}

New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

if (-not $Version) {
    Write-Host "==> resolving the newest stable Microsoft.Web.WebView2 version" -ForegroundColor Cyan
    $index = Invoke-RestMethod "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/index.json"
    $Version = ($index.versions | Where-Object { $_ -notmatch '-' } | Select-Object -Last 1)
}
if (-not $Version) { throw "cannot determine a WebView2 SDK version (no network?)" }
Write-Host "==> WebView2 SDK $Version" -ForegroundColor Cyan

$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"
$nupkg = Join-Path $thirdParty "microsoft.web.webview2.$Version.nupkg"
if (-not (Test-Path $nupkg)) {
    Write-Host "==> downloading $url" -ForegroundColor Cyan
    Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing
}
Write-Host ("==> package: {0:N0} bytes" -f (Get-Item $nupkg).Length) -ForegroundColor DarkGray

# Expand-Archive only accepts a .zip extension, so stage a copy.
$zip = Join-Path $thirdParty "webview2-$Version.zip"
Copy-Item $nupkg $zip -Force
if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
Expand-Archive -Path $zip -DestinationPath $extract -Force
Remove-Item $zip -Force

$native = Join-Path $extract "build/native"
foreach ($required in @("include/WebView2.h", "x64/WebView2Loader.dll", "x64/WebView2Loader.dll.lib")) {
    $path = Join-Path $native $required
    if (-not (Test-Path $path)) { throw "the SDK is missing $required" }
}

$versionFile = Join-Path $extract "VERSION.txt"
Set-Content -Path $versionFile -Value $Version -Encoding ascii

Write-Host "==> WebView2 SDK ready" -ForegroundColor Green
Write-Host "    headers: $native/include"
Write-Host "    loader : $native/x64/WebView2Loader.dll"
Write-Host "    version: $Version"
Write-Host ""
Write-Host "Next: pwsh -File tools/build.ps1" -ForegroundColor DarkGray
