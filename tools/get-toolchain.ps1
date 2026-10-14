# Downloads llvm-mingw (clang + Windows headers/libs, no install needed) into .toolchain\
# build.bat picks it up automatically. Safe to re-run.
$ErrorActionPreference = 'Stop'
$ver  = '20260922'
$name = "llvm-mingw-$ver-ucrt-x86_64"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root '.toolchain'
if (Test-Path (Join-Path $dest "$name\bin\x86_64-w64-mingw32-clang++.exe")) {
    Write-Host "Toolchain already present: .toolchain\$name"
    exit 0
}
New-Item -ItemType Directory -Force $dest | Out-Null
$zip = Join-Path $dest "$name.zip"
Write-Host "Downloading llvm-mingw $ver ..."
$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest "https://github.com/mstorsjo/llvm-mingw/releases/download/$ver/$name.zip" -OutFile $zip
Write-Host "Extracting ..."
if (Get-Command tar.exe -ErrorAction SilentlyContinue) { tar.exe -xf $zip -C $dest } else { Expand-Archive $zip -DestinationPath $dest -Force }
Remove-Item $zip
Write-Host "Done: .toolchain\$name"
