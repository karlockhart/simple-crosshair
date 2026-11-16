@echo off
rem Builds SimpleCrosshair.exe with MinGW (llvm-mingw or MSYS2). No runtime DLLs needed.
rem   build.bat        build only
rem   build.bat sign   build, then sign with Azure Artifact Signing (setup: SIGNING.md)
cd /d "%~dp0"
windres simple-crosshair.rc -O coff -o res.o || exit /b 1
g++ -std=c++17 -O2 -s -static -municode -mwindows -fno-exceptions -fno-rtti ^
    simple-crosshair.cpp res.o -o SimpleCrosshair.exe -lcomdlg32 -lgdi32 -lshell32 -ld2d1 -ldwrite -ldwmapi || exit /b 1
del res.o
echo Built SimpleCrosshair.exe
if /i "%~1"=="sign" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign.ps1" SimpleCrosshair.exe || exit /b 1
)
