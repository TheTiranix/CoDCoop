@echo off
rem ddraw.dll = cargador fijo (si hay juegos abiertos queda el viejo); codcoop_core.dll = nucleo,
rem los juegos abiertos lo recargan solos al ver el archivo nuevo.
cd /d "%~dp0"
set PATH=D:\Tools\mingw32\mingw32\bin;%PATH%
if not exist build mkdir build
set OPT=-std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp
g++ %OPT% -o build\ddraw.dll src\loader.cpp src\ddraw.def
if errorlevel 1 exit /b 1
g++ %OPT% -o build\codcoop_core.dll src\core.cpp -lws2_32
if errorlevel 1 exit /b 1
copy /y build\codcoop_core.dll game\codcoop_core.dll >nul
if errorlevel 1 (echo NO SE PUDO COPIAR el nucleo & exit /b 1)
fc /b build\ddraw.dll game\ddraw.dll >nul 2>nul || copy /y build\ddraw.dll game\ddraw.dll >nul 2>nul || echo AVISO: ddraw.dll en uso, los juegos abiertos siguen con el cargador viejo
rem menu: boton "Cooperativo" (game\Main\pakz_coop.pk3; el juego solo carga pak*.pk3 y el ultimo gana)
python -I tools\make_ui.py >nul 2>nul || echo AVISO: no se pudo armar pakz_coop.pk3 (juego abierto?)
rem instalador: un solo .exe con los 3 archivos del mod y el LEEME adentro (instalador\installer.rc)
windres -i instalador\installer.rc -o build\installer_res.o && g++ -std=c++17 -O2 -static -mwindows -municode -o "build\Instalar CoD Coop.exe" instalador\installer.cpp build\installer_res.o -lole32 -lshell32 || echo AVISO: no se pudo armar el instalador
echo BUILD OK
