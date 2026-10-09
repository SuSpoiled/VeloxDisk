@echo off
setlocal
set ROOT=%~dp0..
cd /d %ROOT%
set VER=1.0.0
set OUT=dist\veloxdisk-setup-%VER%

if not exist build\vd.exe (
    echo Please run build.bat first to produce build\vd.exe
    exit /b 1
)
if not exist build\vd_gui.exe (
    echo Missing build\vd_gui.exe, please run build.bat first
    exit /b 1
)

if exist "%OUT%" rd /s /q "%OUT%"
mkdir "%OUT%"
copy /y build\vd.exe "%OUT%" >nul
if errorlevel 1 goto :err
copy /y build\vd_gui.exe "%OUT%" >nul
if errorlevel 1 goto :err
copy /y install\setup.bat "%OUT%" >nul
if errorlevel 1 goto :err
copy /y install\uninstall.bat "%OUT%" >nul
if errorlevel 1 goto :err
copy /y install\autostart.ps1 "%OUT%" >nul
if errorlevel 1 goto :err
copy /y install\config.default.ini "%OUT%" >nul
if errorlevel 1 goto :err
copy /y install\vd_gui.exe.manifest "%OUT%" >nul
if errorlevel 1 goto :err
copy /y install\README.txt "%OUT%" >nul
if errorlevel 1 goto :err
xcopy /y /i /e /q driver "%OUT%\driver" >nul
if errorlevel 1 goto :err

powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Force -Path '%ROOT%\dist\veloxdisk-setup-%VER%\*' -DestinationPath '%ROOT%\dist\veloxdisk-setup-%VER%.zip'"
if errorlevel 1 goto :err
echo Generated: %ROOT%\dist\veloxdisk-setup-%VER%.zip
exit /b 0

:err
echo Package failed
exit /b 1
