@echo off
setlocal
title OTR Video Switcher Installer

fltmc >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Requesting administrator permission...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

tasklist /FI "IMAGENAME eq obs64.exe" 2>NUL | find /I "obs64.exe" >NUL
if "%errorlevel%"=="0" (
    echo.
    echo Please close OBS Studio before installing OTR Video Switcher.
    echo Then run this installer again.
    echo.
    pause
    exit /b 1
)

set "SOURCE=%~dp0otr-video-switcher"
set "DEST=C:\ProgramData\obs-studio\plugins\otr-video-switcher"

if not exist "%SOURCE%\bin\64bit\otr-video-switcher.dll" (
    echo.
    echo ERROR: The plugin DLL is missing from this package.
    echo Extract the entire ZIP before running the installer.
    echo.
    pause
    exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%"
xcopy "%SOURCE%\*" "%DEST%\" /E /I /Y /Q >NUL
if errorlevel 1 (
    echo.
    echo ERROR: Windows could not copy the plugin into OBS.
    echo.
    pause
    exit /b 1
)

echo.
echo OTR Video Switcher was installed successfully.
echo.
echo Start OBS, click the plus button under Sources, and select:
echo OTR Video Switcher
echo.
pause
exit /b 0
