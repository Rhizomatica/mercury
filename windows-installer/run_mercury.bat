@echo off
title Mercury HF C Modem Console
:: =====================================================================
:: Mercury HF Modem Launcher
:: =====================================================================

cd /d "%~dp0"

echo ===================================================
echo           Launching Mercury HF Modem
echo ===================================================
echo.

:: Check if binary exists
if not exist "mercury.exe" (
    echo [ERROR] mercury.exe binary not found!
    echo Attempting to rebuild or download binary first...
    call install_mercury.bat
    if errorlevel 1 (
        echo [ERROR] mercury.exe missing. Please check your source directory or compile manually.
        pause
        exit /b 1
    )
)

:: Run Mercury HF Modem with linking to mercury.ini configuration
echo Running C binary linked to local mercury.ini configuration:
echo   IP: 127.0.0.1
echo   Port: 8300

echo.

:: Launch the binary with path link to configuration file
mercury.exe -c mercury.ini

if errorlevel 1 (
    echo.
    echo [ERROR] mercury.exe exited with error code: %errorlevel%
    echo Common issues:
    echo 1. The selected COM port in mercury.ini is in use by another SDR or logger program
    echo 2. The Soundcard names selected in mercury.ini do not match "mercury -z" list
    echo.
    pause
)
