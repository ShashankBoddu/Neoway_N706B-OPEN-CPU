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

echo Building HelloWorld...
echo SDK ROOT: %SDK_ROOT%
echo PROJECT DIR: %CUST_PROJ_DIR%

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
cmake "%SDK_ROOT%." -G Ninja -U BUILD_CUST_PROJ -DCMAKE_MAKE_PROGRAM="%NINJA_PATH%" -DCUST_PROJ_DIR="%CUST_PROJ_DIR%"

:: Build
ninja

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    pause
    exit /b %ERRORLEVEL%
)

echo ****************************************************************
echo ***********************  SUCCESS  ******************************
echo ****************************************************************
echo Final binary in SDK: %OUTPUT%\bin\nwy_open_app.bin

:: New Step: Create the ZIP package for Aboot
echo.
echo Packaging for Aboot...
set "ABOOT_TOOL_DIR=%SDK_ROOT%\tools\aboot"
set "BIN_PATH=%OUTPUT%\bin\nwy_open_app.bin"
set "ZIP_OUT=%CUST_PROJ_DIR%\release\nwy_open_app.zip"

if not exist "%CUST_PROJ_DIR%\release" mkdir "%CUST_PROJ_DIR%\release"

:: Run the Aboot packaging tool
cd /d "%ABOOT_TOOL_DIR%"

:: Prepare images (required by the SDK tool)
xcopy /y /b /s "configurations\releasepack-ASR1605-source\images\ASR1605S_LTEOnly_SMS_DataModule" "configurations\releasepack-ASR1605-source\images\"

arelease.exe -c configurations\releasepack-ASR1605-source -g -p ASR1605_EVB -v ASR1605_SMS_04MB -i app="%BIN_PATH%" "%ZIP_OUT%"

if %ERRORLEVEL% NEQ 0 (
    echo Packaging failed!
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo ****************************************************************
echo SUCCESS: ZIP package created for Aboot!
echo Location: %ZIP_OUT%
echo ****************************************************************
pause
