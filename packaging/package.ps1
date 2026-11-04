# Builds Release and makes the package: dist\EFMIVulkan\ and dist\EFMIVulkan-<version>.zip.
#
#   powershell -ExecutionPolicy Bypass -File packaging\package.ps1 -Version 0.1.0
param([string]$Version = "0.1.0")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path build\CMakeCache.txt)) { cmake -S . -B build -G "Visual Studio 17 2022" -A x64 }
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$out = Join-Path $root "dist\EFMIVulkan"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$release = Join-Path $root "build\Release"
Copy-Item "$release\efmivulkan.dll", "$release\efmivulkan.json" $out
Copy-Item "$release\efmivulkan_launch.exe" "$out\EFMIVulkan.exe"
Copy-Item "$PSScriptRoot\README.txt" $out
if (Test-Path "$root\LICENSE") { Copy-Item "$root\LICENSE" "$out\LICENSE.txt" }
Set-Content -Path "$out\VERSION.txt" -Value $Version -Encoding ascii

$zip = Join-Path $root "dist\EFMIVulkan-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $out -DestinationPath $zip
Write-Host "Package: $out"
Write-Host "Zip:     $zip"
