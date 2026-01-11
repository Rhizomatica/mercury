@echo off
REM ============================================================================
REM Mercury Audio Device Configuration Helper
REM This script helps you select the correct audio devices for your radio
REM ============================================================================

setlocal enabledelayedexpansion

cls
echo.
echo ========================================================================
echo   MERCURY AUDIO DEVICE CONFIGURATION
echo ========================================================================
echo.
echo This script will help you configure Mercury to use the correct
echo audio devices for your radio interface.
echo.
echo Press any key to continue...
pause >nul

REM Find mercury.exe
set "MERCURY_EXE="
if exist "mercury.exe" (
    set "MERCURY_EXE=mercury.exe"
) else if exist "x:\Storage\Documents\hermes and mercury\mercury\mercury.exe" (
    set "MERCURY_EXE=x:\Storage\Documents\hermes and mercury\mercury\mercury.exe"
) else if exist "%ProgramFiles%\Mercury\mercury.exe" (
    set "MERCURY_EXE=%ProgramFiles%\Mercury\mercury.exe"
)

if "%MERCURY_EXE%"=="" (
    echo.
    echo ERROR: mercury.exe not found!
    echo Please run this script from the Mercury directory.
    echo.
    pause
    exit /b 1
)

REM Find config file location (mercury_config.ini for Winlink setup)
set "CONFIG_FILE="
if exist "%ProgramFiles%\Mercury\config\mercury_config.ini" (
    set "CONFIG_FILE=%ProgramFiles%\Mercury\config\mercury_config.ini"
) else if exist "..\winlink-setup\mercury_config.ini" (
    set "CONFIG_FILE=..\winlink-setup\mercury_config.ini"
) else if exist "winlink-setup\mercury_config.ini" (
    set "CONFIG_FILE=winlink-setup\mercury_config.ini"
)

if "%CONFIG_FILE%"=="" (
    echo.
    echo ERROR: mercury_config.ini not found!
    echo Expected locations:
    echo   - %ProgramFiles%\Mercury\config\mercury_config.ini
    echo   - ..\winlink-setup\mercury_config.ini
    echo.
    pause
    exit /b 1
)

cls
echo.
echo ========================================================================
echo   DETECTING AUDIO DEVICES
echo ========================================================================
echo.
echo Scanning for available audio devices...
echo.

REM Create temp file for device list
set "TEMP_DEVICES=%TEMP%\mercury_audio_devices.txt"
"%MERCURY_EXE%" -z > "%TEMP_DEVICES%" 2>&1

echo.
echo Available devices:
echo.
type "%TEMP_DEVICES%"
echo.
echo ========================================================================
echo.

echo.
echo INSTRUCTIONS:
echo.
echo 1. Look at the device names listed above
echo 2. Find your radio's audio interface (e.g., "USB Audio CODEC")
echo 3. You'll need BOTH the capture and playback device names
echo 4. They usually have similar names
echo.
echo Common radio interfaces:
echo   - USB Audio CODEC (generic USB sound cards)
echo   - SignaLink USB (SignaLink interfaces)
echo   - RigBlaster (RigBlaster interfaces)
echo   - Digirig (Digirig interfaces)
echo.
echo NOTE: "Primary Sound Driver" and "Speakers" are usually your PC audio,
echo       NOT your radio interface!
echo.
pause

cls
echo.
echo ========================================================================
echo   CONFIGURE AUDIO DEVICES
echo ========================================================================
echo.

REM Get capture device
echo.
echo Enter the CAPTURE device name for your radio:
echo (This is usually the microphone/input device)
echo.
echo Example: Microphone (USB Audio CODEC )
echo.
echo Or press ENTER to use "default"
echo.
set /p "CAPTURE_DEVICE=Capture device: "
if "%CAPTURE_DEVICE%"=="" set "CAPTURE_DEVICE=default"

echo.
echo Enter the PLAYBACK device name for your radio:
echo (This is usually the speaker/output device)
echo.
echo Example: Speakers (USB Audio CODEC )
echo.
echo Or press ENTER to use "default"
echo.
set /p "PLAYBACK_DEVICE=Playback device: "
if "%PLAYBACK_DEVICE%"=="" set "PLAYBACK_DEVICE=default"

cls
echo.
echo ========================================================================
echo   CONFIGURATION SUMMARY
echo ========================================================================
echo.
echo Capture Device:  %CAPTURE_DEVICE%
echo Playback Device: %PLAYBACK_DEVICE%
echo.
echo Config File: %CONFIG_FILE%
echo.
echo ========================================================================
echo.
echo This configuration will be written to mercury_config.ini
echo.
set /p "CONFIRM=Is this correct? (Y/N): "
if /i not "%CONFIRM%"=="Y" (
    echo.
    echo Configuration cancelled.
    pause
    exit /b 0
)

REM Update config file - handle INI format with [RADIO] section
echo.
echo Updating configuration...

set "TEMP_CONFIG=%TEMP%\mercury_config_temp.txt"
set "IN_RADIO_SECTION=0"

(
    for /f "usebackq delims=" %%a in ("%CONFIG_FILE%") do (
        set "LINE=%%a"

        REM Check if entering [RADIO] section
        if "!LINE!"=="[RADIO]" set "IN_RADIO_SECTION=1"

        REM Check if leaving [RADIO] section (new section starts)
        if "!LINE:~0,1!"=="[" if not "!LINE!"=="[RADIO]" set "IN_RADIO_SECTION=0"

        REM Replace device lines in [RADIO] section
        if "!IN_RADIO_SECTION!"=="1" (
            if not "!LINE:CaptureDevice=!"=="!LINE!" (
                echo CaptureDevice=!CAPTURE_DEVICE!
            ) else if not "!LINE:PlaybackDevice=!"=="!LINE!" (
                echo PlaybackDevice=!PLAYBACK_DEVICE!
            ) else (
                echo !LINE!
            )
        ) else (
            echo !LINE!
        )
    )
) > "%TEMP_CONFIG%"

REM Replace config file
move /Y "%TEMP_CONFIG%" "%CONFIG_FILE%" >nul

echo.
echo ========================================================================
echo   CONFIGURATION COMPLETE!
echo ========================================================================
echo.
echo Audio devices have been configured in mercury_config.ini
echo.
echo Capture:  %CAPTURE_DEVICE%
echo Playback: %PLAYBACK_DEVICE%
echo.
echo NEXT STEPS:
echo.
echo 1. Restart Mercury for changes to take effect
echo 2. Test the configuration with Winlink
echo 3. Make sure your radio is set to DATA mode
echo 4. Check audio levels (shouldn't clip/distort)
echo.
echo ========================================================================
echo.

REM Cleanup
if exist "%TEMP_DEVICES%" del /q "%TEMP_DEVICES%"

pause
