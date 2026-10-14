#!/bin/sh
# Cross-compile from Linux/WSL/macOS with llvm-mingw on PATH (https://github.com/mstorsjo/llvm-mingw).
# Usage: ./build.sh [debug]
set -e
cd "$(dirname "$0")"
OPT="-O2 -s"; [ "$1" = debug ] && OPT="-O0 -g"
mkdir -p bin
(cd src && x86_64-w64-mingw32-windres tessera.rc -O coff -o ../bin/tessera_res.o)
x86_64-w64-mingw32-clang++ -std=c++17 $OPT -municode -mwindows -static \
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  src/main.cpp bin/tessera_res.o -o bin/Tessera.exe \
  -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -luuid -lshlwapi -lshell32 -ldwmapi -lcomdlg32 -lgdi32 -luser32
rm -f bin/tessera_res.o
echo "Built bin/Tessera.exe"
