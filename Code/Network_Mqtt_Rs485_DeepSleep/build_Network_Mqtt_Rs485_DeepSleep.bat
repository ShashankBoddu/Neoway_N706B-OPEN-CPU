@echo off
setlocal

:: Path to the SDK root
set "SDK_ROOT=E:\projects\DevelopmentLevelCode\Neoway_N706B\N706B-A07-STD-OE_CN1X_ITRI-009_SDK\N706B-A07-STD-OE_CN1X_ITRI-009_SDK"
:: Current project directory
set "CUST_PROJ_DIR=%~dp0"
:: Remove trailing backslash if present
if "%CUST_PROJ_DIR:~-1%"=="\" set "CUST_PROJ_DIR=%CUST_PROJ_DIR:~0,-1%"

:: Build output dir
set "OUTPUT=%SDK_ROOT%\out"

echo ================================================================
echo Building Network_Mqtt_Rs485_DeepSleep...
echo SDK ROOT: %SDK_ROOT%
echo PROJECT DIR: %CUST_PROJ_DIR%
echo ================================================================

:: Change to SDK directory to run the build
cd /d "%SDK_ROOT%"

:: Initialize environment
call tools\core_launch.bat

:: Clean old out folder to avoid cache issues
if exist "%OUTPUT%" (
    echo Cleaning old build artifacts...
    rd /s /q "%OUTPUT%"
)
mkdir "%OUTPUT%"
cd /d "%OUTPUT%"

:: Run CMake with our custom project variable
set "NINJA_PATH=%SDK_ROOT%\prebuilts\win32\bin\ninja.exe"
cmake "%SDK_ROOT%." -G Ninja -U BUILD_CUST_PROJ -DCMAKE_MAKE_PROGRAM="%NINJA_PATH%" -DCUST_PROJ_DIR="%CUST_PROJ_DIR%" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

:: Build
ninja

if %ERRORLEVEL% NEQ 0 (
    echo ================================================================
    echo Build failed!
    echo ================================================================
    exit /b %ERRORLEVEL%
)

:: Copy compile_commands.json to workspace root for Clangd
if exist "%OUTPUT%\compile_commands.json" (
    copy /y "%OUTPUT%\compile_commands.json" "%CUST_PROJ_DIR%\..\compile_commands.json" > nul
)

echo ****************************************************************
echo ***********************  SUCCESS  ******************************
echo ****************************************************************
echo Final binary in SDK: %OUTPUT%\bin\nwy_open_app.bin

if not exist "%CUST_PROJ_DIR%\release" mkdir "%CUST_PROJ_DIR%\release"

:: Step 1: Create FOTA PKT package (.pkt)
echo.
echo Packaging FOTA Update (.pkt)...
cd /d "%SDK_ROOT%\tools"
copy /y "%OUTPUT%\bin\nwy_open_app.bin" "nwy_open_app.bin" > nul
nwy_mkpackage.exe
copy /y "nwy_open_app_fota.pkt" "%CUST_PROJ_DIR%\release\nwy_open_app_fota.pkt" > nul
del "nwy_open_app.bin" > nul 2>&1
del "nwy_open_app_fota.pkt" > nul 2>&1
echo Location: %CUST_PROJ_DIR%\release\nwy_open_app_fota.pkt

:: Step 2: Create ZIP package for Aboot (.zip)
echo.
echo Packaging for Aboot (.zip)...
set "ABOOT_TOOL_DIR=%SDK_ROOT%\tools\aboot"
set "BIN_PATH=%OUTPUT%\bin\nwy_open_app.bin"
set "ZIP_OUT=%CUST_PROJ_DIR%\release\nwy_open_app.zip"

cd /d "%ABOOT_TOOL_DIR%"

:: Prepare images (required by the SDK tool)
xcopy /y /b /s "configurations\releasepack-ASR1605-source\images\ASR1605S_LTEOnly_SMS_DataModule" "configurations\releasepack-ASR1605-source\images\" > nul

arelease.exe -c configurations\releasepack-ASR1605-source -g -p ASR1605_EVB -v ASR1605_SMS_04MB -i app="%BIN_PATH%" "%ZIP_OUT%"

if %ERRORLEVEL% NEQ 0 (
    echo Packaging failed!
    exit /b %ERRORLEVEL%
)

echo.
echo ****************************************************************
echo SUCCESS: Packages generated!
echo ZIP: %ZIP_OUT%
echo FOTA PKT: %CUST_PROJ_DIR%\release\nwy_open_app_fota.pkt
echo ****************************************************************
