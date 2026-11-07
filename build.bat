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
rem CoD2 (en prueba): modulo aparte que carga ddraw.dll en CoD2SP_s.exe; copia de desarrollo en game2\
g++ %OPT% -o build\cod2coop.dll src\cod2.cpp
if errorlevel 1 exit /b 1
if exist game2\CoD2SP_s.exe copy /y build\cod2coop.dll game2\ >nul && copy /y build\ddraw.dll game2\ >nul
copy /y build\codcoop_core.dll game\codcoop_core.dll >nul
if errorlevel 1 (echo NO SE PUDO COPIAR el nucleo & exit /b 1)
fc /b build\ddraw.dll game\ddraw.dll >nul 2>nul || copy /y build\ddraw.dll game\ddraw.dll >nul 2>nul || echo AVISO: ddraw.dll en uso, los juegos abiertos siguen con el cargador viejo
rem menu: boton "Cooperativo" (game\Main\pakz_coop.pk3; el juego solo carga pak*.pk3 y el ultimo gana)
python -I tools\make_ui.py >nul 2>nul || echo AVISO: no se pudo armar pakz_coop.pk3 (juego abierto?)
rem Steam (Spacewar 480): steam_api.dll de 32 bits, el redistribuible que trae SteamVR (no va en el repo)
if not exist build\steam_api.dll copy /y "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win32\steam_api.dll" build\ >nul
fc /b build\steam_api.dll game\steam_api.dll >nul 2>nul || copy /y build\steam_api.dll game\ >nul 2>nul || echo AVISO: no se pudo copiar steam_api.dll a game\
rem instalador: un solo .exe con los archivos del mod, steam_api.dll y el README.txt adentro (installer\installer.rc)
windres -i installer\installer.rc -o build\installer_res.o && g++ -std=c++17 -O2 -static -mwindows -municode -o "build\Install CoD Coop.exe" installer\installer.cpp build\installer_res.o -lole32 -lshell32 || echo AVISO: no se pudo armar el instalador
echo BUILD OK
