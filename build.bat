@echo off
rem Builds bin\Tessera.exe with llvm-mingw (clang). Usage:  build.bat          (release)
rem                                                          build.bat debug    (symbols, no optimisation)
rem First time only:  powershell -ExecutionPolicy Bypass -File tools\get-toolchain.ps1
setlocal
cd /d "%~dp0"

set "TC="
for /d %%D in (".toolchain\llvm-mingw-*") do set "TC=%%~fD\bin"
if defined TC set "PATH=%TC%;%PATH%"
where x86_64-w64-mingw32-clang++ >nul 2>nul || (
  echo No compiler found. Run:  powershell -ExecutionPolicy Bypass -File tools\get-toolchain.ps1
  exit /b 1
)

set "OPT=-O2 -s"
if /i "%~1"=="debug" set "OPT=-O0 -g"

if not exist bin mkdir bin
pushd src
x86_64-w64-mingw32-windres tessera.rc -O coff -o ..\bin\tessera_res.o || (popd & exit /b 1)
x86_64-w64-mingw32-clang++ -std=c++17 %OPT% -municode -mwindows -static ^
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers ^
  main.cpp ..\bin\tessera_res.o -o ..\bin\Tessera.exe ^
  -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -luuid -lshlwapi -lshell32 -ldwmapi -lcomdlg32 -lgdi32 -luser32 ^
  || (popd & exit /b 1)
popd
del bin\tessera_res.o
echo Built bin\Tessera.exe
