@echo off
chcp 65001 >nul
setlocal EnableExtensions
title VeloxDisk 1.0.0 安装程序

set "INST=%ProgramFiles%\VeloxDisk"
set "CFGDIR=%LOCALAPPDATA%\VeloxDisk"
set "SM=%ProgramData%\Microsoft\Windows\Start Menu\Programs\VeloxDisk.lnk"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo 安装需要管理员权限, 正在请求 UAC 提权...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Start-Process -FilePath '%~f0' -Verb RunAs } catch { exit 1 }" <nul
    if %errorlevel% neq 0 (
        echo UAC 被拒绝或提权失败, 安装取消。
        pause
        exit /b 1
    )
    echo 已弹出管理员安装窗口, 请在新窗口中继续。
    pause
    exit /b 0
)

set "AUTO=%~1"
if not defined AUTO (
    set /p AUTO=启用开机自启动? 登录时自动启动到托盘, 最高权限 Y/N
)
set "AUTO=%AUTO: =%"

echo.
echo ==============================================
echo   VeloxDisk 1.0.0 安装
echo   程序目录 : %INST%
echo   配置文件 : %CFGDIR%\config.ini
echo ==============================================
echo.

taskkill /F /IM vd.exe >nul 2>&1
taskkill /F /IM vd_gui.exe >nul 2>&1

if not exist "%INST%" mkdir "%INST%"

rem 发行包若被直接解压到程序目录, 拷贝会变成"复制到自身"而失败, 此时跳过文件拷贝
if /i not "%~dp0"=="%INST%\" (
    copy /y "%~dp0vd.exe" "%INST%\" >nul
    if errorlevel 1 goto :fail
    copy /y "%~dp0vd_gui.exe" "%INST%\" >nul
    if errorlevel 1 goto :fail
    copy /y "%~dp0vd_gui.exe.manifest" "%INST%\" >nul
    if errorlevel 1 goto :fail
    if exist "%~dp0driver" xcopy /y /i /e /q "%~dp0driver" "%INST%\driver" >nul
    if errorlevel 1 goto :fail
    copy /y "%~dp0uninstall.bat" "%INST%\" >nul
    if errorlevel 1 goto :fail
    copy /y "%~dp0autostart.ps1" "%INST%\" >nul
    if errorlevel 1 goto :fail
)

if not exist "%CFGDIR%" mkdir "%CFGDIR%"
if not exist "%CFGDIR%\config.ini" (
    copy /y "%~dp0config.default.ini" "%CFGDIR%\config.ini" >nul
    if errorlevel 1 goto :fail
)

reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /v DisplayName /d "VeloxDisk" /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /v DisplayVersion /d "1.0.0" /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /v Publisher /d "VeloxDisk" /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /v DisplayIcon /d "%INST%\vd_gui.exe" /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /v UninstallString /d "\"%INST%\uninstall.bat\"" /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /v NoModify /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /v NoRepair /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /v InstallDate /d "2026-02-06" /f >nul

powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%SM%'); $s.TargetPath='%INST%\vd_gui.exe'; $s.WorkingDirectory='%INST%'; $s.Description='VeloxDisk user-mode disk cache'; $s.Save()" >nul 2>&1 <nul

if /i not "%AUTO%"=="Y" (
    echo 未启用开机自启动, 之后可在托盘菜单中开启。
    goto :done
)
rem 用任务 XML 注册自启动任务 (autostart.ps1): schtasks /TR 的命令行解析
rem 会把含空格的路径在空格处拆散, %ProgramFiles% 必含空格, 登录启动报"文件找不到"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0autostart.ps1" -ExePath "%INST%\vd_gui.exe"
if %errorlevel% neq 0 (
    echo 创建开机自启动任务失败, 其余部分不受影响。
) else (
    reg add "HKCU\Software\VeloxDisk" /v AutoMode /t REG_DWORD /d 2 /f >nul
    echo 开机自启动已启用: 登录时以最高权限自动启动到托盘。
)

:done
echo.
echo ============ 安装完成 ============
echo 程序目录: %INST%
echo 配置文件: %CFGDIR%\config.ini
echo 可从开始菜单或双击 %INST%\vd_gui.exe 启动。
echo.
pause
exit /b 0

:fail
echo.
echo 安装失败! (文件拷贝出错, 请确认发行包完整且未被解压到 %INST% 之外缺少文件的目录)
pause
exit /b 1
