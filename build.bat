@echo off
setlocal

echo Compiling resource file...
windres remap.rc -O coff -o remap_res.o
if %errorlevel% neq 0 exit /b %errorlevel%

echo Compiling with MinGW.
g++ -std=c++17 -O2 -municode -mwindows -static remap.cpp remap_res.o -o remap.exe -lgdi32 -lhid -lsetupapi -lcomdlg32

echo Build complete: remap.exe