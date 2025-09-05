@echo off
echo Uninstalling fmgNICE Video Plugin from OBS Studio...
echo.

REM Set path
set OBS_PLUGIN_DIR=C:\ProgramData\obs-studio\plugins\fmgnice-video

REM Check if plugin is installed
if not exist "%OBS_PLUGIN_DIR%" (
    echo Plugin is not installed.
    pause
    exit /b 0
)

REM Remove plugin directory
echo Removing plugin files...
rmdir /S /Q "%OBS_PLUGIN_DIR%"
if %errorlevel% neq 0 (
    echo Failed to remove plugin files. You may need administrator privileges.
    pause
    exit /b %errorlevel%
)

echo.
echo ========================================
echo Uninstallation completed successfully!
echo ========================================
echo.
echo Please restart OBS Studio if it's currently running.
echo.
pause