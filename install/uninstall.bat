@echo off
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion
title VeloxDisk 卸载程序

set "INST=%ProgramFiles%\VeloxDisk"
set "CFGDIR=%LOCALAPPDATA%\VeloxDisk"
set "SM=%ProgramData%\Microsoft\Windows\Start Menu\Programs\VeloxDisk.lnk"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo 卸载需要管理员权限, 正在请求 UAC 提权...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs" <nul
    exit /b 0
)

set "CONF="
set /p CONF=确认卸载 VeloxDisk? Y/N
set "CONF=%CONF: =%"
if /i not "%CONF%"=="Y" (
    echo 已取消卸载。
    pause
    exit /b 0
)

set "LDLIST="
if exist "%CFGDIR%\config.ini" (
    for /f "delims=" %%L in ('findstr "l2Dir" "%CFGDIR%\config.ini" 2^>nul') do (
        set "D=%%L"
        for /f "tokens=* delims= " %%F in ("!D!") do set "F=%%F"
        if "!F:~0,1!" neq ";" (
            for /f "tokens=2 delims==" %%V in ("!D!") do set "D=%%V"
            for /f "tokens=* delims= " %%W in ("!D!") do set "D=%%W"
            if defined D (
                if exist "!D!" (
                    set "LD="
                    set /p LD=删除缓存数据目录 !D! Y/N
                    if /i "!LD!"=="Y" set "LDLIST=!LDLIST!|!D!"
                )
            )
        )
    )
)

echo.
echo ==============================================
echo   卸载 VeloxDisk 1.0.0
echo   程序目录: %INST%
echo   配置目录: %CFGDIR%
echo   另将删除: 计划任务 / 注册表项 / 开始菜单快捷方式
echo ==============================================
echo.

echo [1/8] 删除计划任务 VeloxDisk...
schtasks /Delete /TN VeloxDisk /F >nul 2>&1

echo [2/8] 结束正在运行的 vd.exe / vd_gui.exe...
taskkill /F /IM vd.exe >nul 2>&1
taskkill /F /IM vd_gui.exe >nul 2>&1

echo [3/8] 删除注册表项 (含所有用户 HKU)...
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VeloxDisk" /f >nul 2>&1
reg delete "HKCU\Software\VeloxDisk" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\VeloxDisk" /f >nul 2>&1
for /f "tokens=2*" %%U in ('reg query "HKU" 2^>nul') do reg delete "HKU\%%U\Software\VeloxDisk" /f >nul 2>&1

echo [4/8] 删除开始菜单快捷方式...
del /f /q "%SM%" >nul 2>&1

echo [5/8] 删除已确认的缓存数据目录...
if defined LDLIST (
    for /f "tokens=1* delims=|" %%D in ("!LDLIST!") do (
        if exist "%%D\" rd /s /q "%%D" >nul 2>&1
        if exist "%%D" del /f /q "%%D" >nul 2>&1
    )
)

echo [6/8] 删除程序目录文件...
if exist "%LOCALAPPDATA%\Programs\VeloxDisk\vd.exe" rd /s /q "%LOCALAPPDATA%\Programs\VeloxDisk" >nul 2>&1
del /f /q "%INST%\vd.exe" "%INST%\vd_gui.exe" "%INST%\vd_gui.exe.manifest" >nul 2>&1
rmdir /s /q "%INST%\driver" >nul 2>&1

echo [7/8] 删除配置目录...
if exist "%CFGDIR%" rd /s /q "%CFGDIR%" >nul 2>&1

echo [8/8] 清理程序目录剩余内容 (含本脚本)...
echo.
echo ============ 卸载完成 ============
echo VeloxDisk 及其全部相关设置已移除。
echo.
pause
start "" /b powershell -NoProfile -Command "$p='%INST%'; 1..5 | ForEach-Object { Start-Sleep 1; if (Test-Path $p) { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue } }" <nul >nul 2>&1
exit /b 0
