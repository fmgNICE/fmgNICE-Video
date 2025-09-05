@echo off
echo Installing fmgNICE Video Plugin to OBS Studio...
echo.

REM Set paths
set PLUGIN_DLL=build\Release\fmgnice-video.dll
set DATA_DIR=data
set OBS_PLUGIN_DIR=C:\ProgramData\obs-studio\plugins\fmgnice-video
set OBS_BIN_DIR=%OBS_PLUGIN_DIR%\bin\64bit
set OBS_DATA_DIR=%OBS_PLUGIN_DIR%\data

REM Check if DLL exists
if not exist "%PLUGIN_DLL%" (
    echo Error: Plugin DLL not found at %PLUGIN_DLL%
    echo Please build the plugin first.
    pause
    exit /b 1
)

REM Create directories
echo Creating plugin directories...
if not exist "%OBS_PLUGIN_DIR%" mkdir "%OBS_PLUGIN_DIR%"
if not exist "%OBS_BIN_DIR%" mkdir "%OBS_BIN_DIR%"
if not exist "%OBS_DATA_DIR%" mkdir "%OBS_DATA_DIR%"

REM Copy plugin DLL
echo Copying plugin DLL...
copy /Y "%PLUGIN_DLL%" "%OBS_BIN_DIR%\fmgnice-video.dll"
if %errorlevel% neq 0 (
    echo Failed to copy plugin DLL. You may need administrator privileges.
    pause
    exit /b %errorlevel%
)

REM Copy data files
if exist "%DATA_DIR%" (
    echo Copying data files...
    xcopy /Y /E /I "%DATA_DIR%\*" "%OBS_DATA_DIR%\"
    if %errorlevel% neq 0 (
        echo Warning: Failed to copy some data files.
    )
)

echo.
echo ========================================
echo Installation completed successfully!
echo ========================================
echo.
echo Plugin installed to: %OBS_PLUGIN_DIR%
echo.
echo Please restart OBS Studio to use the fmgNICE Video Plugin.
echo You will find "fmgNICE Video Source" in the sources list.
echo.
pause