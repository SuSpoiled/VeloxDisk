@echo off
setlocal
set VC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VC% (
  echo error: vcvars64.bat not found at %VC%
  exit /b 1
)
call %VC% >nul
cd /d %~dp0
if not exist build mkdir build

set COMMON=src\common\log.cpp src\common\util.cpp src\common\config.cpp src\common\stats.cpp
set CACHE=src\cache\key.cpp src\cache\numa.cpp src\cache\source.cpp src\cache\ramtier.cpp src\cache\ssdtier.cpp src\cache\tieredcache.cpp
set ENGINE=src\engine\engine.cpp
set RAMDISK=src\ramdisk\ramdisk.cpp
set UIMMI=src\uimmi\uimmi.cpp

echo === building vd.exe ===
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32 /Fo:build\ /Fe:build\vd.exe %COMMON% %CACHE% %ENGINE% %RAMDISK% %UIMMI% src\cli\main.cpp /link /SUBSYSTEM:CONSOLE shell32.lib advapi32.lib
if errorlevel 1 goto :err

if not exist build\gui mkdir build\gui
echo === building vd_gui.exe ===
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32 /Fo:build\gui\ /Fe:build\vd_gui.exe %COMMON% %CACHE% %ENGINE% %RAMDISK% %UIMMI% src\gui\main_gui.cpp /link /SUBSYSTEM:WINDOWS kernel32.lib user32.lib gdi32.lib shell32.lib comdlg32.lib comctl32.lib advapi32.lib ole32.lib oleaut32.lib taskschd.lib
if errorlevel 1 goto :err

if not exist build\test mkdir build\test
if not exist build\demo mkdir build\demo
echo === building vd_demo.exe ===
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32 /Fo:build\demo\ /Fe:build\vd_demo.exe %COMMON% %CACHE% %ENGINE% %RAMDISK% %UIMMI% tests\demo_main.cpp /link /SUBSYSTEM:CONSOLE shell32.lib advapi32.lib
if errorlevel 1 goto :err
echo.

echo === building vd_tests.exe ===
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32 /Fo:build\test\ /Fe:build\vd_tests.exe %COMMON% %CACHE% %ENGINE% %RAMDISK% %UIMMI% tests\test_main.cpp /link /SUBSYSTEM:CONSOLE shell32.lib advapi32.lib
if errorlevel 1 goto :err

echo === running vd_tests.exe ===
build\vd_tests.exe
if errorlevel 1 goto :err

echo BUILD OK
exit /b 0
:err
echo BUILD FAILED
exit /b 1
