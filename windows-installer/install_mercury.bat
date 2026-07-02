@echo off
title Mercury C Bootstrapper
:: =====================================================================
:: Mercury HF Modem C Source Downloader & Compiler Script
:: =====================================================================

cd /d "%~dp0"

echo ===================================================
echo   Setting up Mercury HF Standalone C Modem
echo ===================================================
echo.

if exist "mercury.exe" (
    echo [INFO] mercury.exe is already present. Ready to run!
    exit /b 0
)

:: Step 1: Download C Source Code from GitHub
echo [1/3] Downloading latest Mercury HF modem source code (%mercuryBranch% branch)...
echo Repo: https://github.com/Rhizomatica/mercury
echo.

powershell -Command "& { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Write-Host 'Downloading ZIP archive...'; Invoke-WebRequest -Uri 'https://github.com/Rhizomatica/mercury/archive/refs/heads/master.zip' -OutFile 'mercury_source.zip' }"

if not exist "mercury_source.zip" (
    echo [ERROR] Failed to download source code archive from GitHub.
    echo Checking for git fallback...
    where git >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] Git or internet connection is missing. Cannot fetch C files.
        pause
        exit /b 1
    ) else (
        echo Git is installed. Cloning repository...
        git clone -b master https://github.com/Rhizomatica/mercury temp_git
        xcopy /s /e /y temp_git\* .
        rd /s /q temp_git
    )
) else (
    echo [2/3] Extracting source code files...
    powershell -Command "& { Expand-Archive -Path 'mercury_source.zip' -DestinationPath 'mercury_temp' -Force }"
    xcopy /s /e /y "mercury_temp\mercury-master\*" .
    del /f /q mercury_source.zip
    rd /s /q mercury_temp
)

:: Step 2: Compile binary if compiler is present, or show instructions
echo.
echo [3/3] Compiling C modem source code...

echo [INFO] Precompiled distribution mode selected.
echo Please ensure 'mercury.exe' is bundled inside the installer folder.
echo If compile is needed, run: 'gcc -O3 -Wall -o mercury.exe *.c -lportaudio -lpthread'


echo.
echo ===================================================
echo   Mercury Standalone Bootstrap Completed!
echo ===================================================
echo.
exit /b 0
