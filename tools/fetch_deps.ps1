# fetch_deps.ps1 - Prepare third-party dependencies for Windows (MinGW-w64).
#
# Downloads the FFmpeg shared dev package (headers + .dll.a import libs + DLLs)
# into third_party/ffmpeg. The project already bundles a working curl dev
# package under third_party/curl; if you delete it, install it via MSYS2:
#     pacman -S mingw-w64-x86_64-curl
# and point CURL_ROOT in CMakeLists.txt at that location, or re-bundle it.
#
# Usage (PowerShell):  .\tools\fetch_deps.ps1

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$ThirdParty = Join-Path $Root "third_party"
$FFmpegDir = Join-Path $ThirdParty "ffmpeg"

$Url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip"

if (Test-Path $FFmpegDir) {
    Write-Host "third_party/ffmpeg already exists - skipping FFmpeg download."
    exit 0
}

New-Item -ItemType Directory -Force -Path $ThirdParty | Out-Null
$tmpZip = Join-Path $env:TEMP "ffmpeg-shared.zip"

Write-Host "Downloading FFmpeg shared dev package ..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $Url -OutFile $tmpZip -UseBasicParsing

Write-Host "Extracting ..."
$tmpExtract = Join-Path $env:TEMP "ffmpeg-shared-extract"
if (Test-Path $tmpExtract) { Remove-Item -Recurse -Force $tmpExtract }
Expand-Archive -Path $tmpZip -DestinationPath $tmpExtract

# The archive nests under ffmpeg-master-latest-win64-gpl-shared/
$nested = Join-Path $tmpExtract "ffmpeg-master-latest-win64-gpl-shared"
New-Item -ItemType Directory -Force -Path $FFmpegDir | Out-Null
Copy-Item -Path (Join-Path $nested "*") -Destination $FFmpegDir -Recurse

Remove-Item -Force $tmpZip
Remove-Item -Recurse -Force $tmpExtract

Write-Host "FFmpeg dev package ready at $FFmpegDir"
Write-Host "Make sure a MinGW-w64 toolchain (gcc/g++) is on PATH, then:"
Write-Host "  cmake -G ""MinGW Makefiles"" -S . -B build"
Write-Host "  cmake --build build"
