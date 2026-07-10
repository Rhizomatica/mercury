@echo off
title Mercury HF Modem
:: =====================================================================
:: Mercury HF Modem Launcher — single-binary: engine + UI in one.
:: =====================================================================

cd /d "%~dp0"

if not exist "mercury-ui.exe" (
    echo [ERROR] mercury-ui.exe not found!
    echo Please reinstall Mercury HF Modem.
    pause
    exit /b 1
)

echo Starting Mercury HF Modem...
start "" "mercury-ui.exe"
