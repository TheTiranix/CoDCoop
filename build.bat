@echo off
cd /d "%~dp0"
set PATH=D:\Tools\mingw32\mingw32\bin;%PATH%
if not exist build mkdir build
g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -o build\ddraw.dll src\main.cpp src\coop.cpp src\ddraw.def -lws2_32 -lwinmm
if errorlevel 1 exit /b 1
copy /y build\ddraw.dll game\ddraw.dll >nul
if errorlevel 1 (echo NO SE PUDO COPIAR: el juego esta abierto? & exit /b 1)
echo BUILD OK
