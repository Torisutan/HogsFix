# Builds the release zip: build\HogsFix-<version>.zip with dinput.dll (Ultimate ASI Loader), HogsFix.asi, HogsFix.ini,
# README.txt and the loader's license. Run build.bat first.
param([string]$Version = "0.1")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot; $build = Join-Path $root "build"; $stage = Join-Path $build "stage"
$ualUrl = "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader.zip"
$ualSha = "d5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6"   # dinput8.dll (x86) in that zip
if (-not (Test-Path "$build\HogsFix.asi")) { throw "build\HogsFix.asi missing - run build.bat" }
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $stage | Out-Null
$ualZip = Join-Path $build "Ultimate-ASI-Loader-9.7.4.zip"
if (-not (Test-Path $ualZip)) { Invoke-WebRequest $ualUrl -OutFile $ualZip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($ualZip)
try { [IO.Compression.ZipFileExtensions]::ExtractToFile(($z.Entries | Where-Object Name -eq "dinput8.dll"), "$stage\dinput.dll") } finally { $z.Dispose() }
if ((Get-FileHash "$stage\dinput.dll" -Algorithm SHA256).Hash -ne $ualSha) { throw "dinput8.dll hash mismatch" }
Copy-Item "$build\HogsFix.asi", "$root\dist\HogsFix.ini", "$root\dist\README.txt", "$root\dist\LICENSE-UltimateASILoader.txt" $stage
$out = Join-Path $build "HogsFix-$Version.zip"
Remove-Item $out -ErrorAction SilentlyContinue
Compress-Archive "$stage\*" $out
Get-FileHash $out -Algorithm SHA256 | ForEach-Object { "$out`n$($_.Hash)" }
