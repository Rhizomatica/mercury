@echo off
echo ===== Mercury Gearshift Test =====
echo.
echo This will test Mercury ARQ mode with gearshift enabled.
echo Press Ctrl+C to stop the test after observing initialization.
echo.
echo Starting Mercury with -g flag (gearshift enabled)...
echo.

mercury.exe -m ARQ -r stockhf -g -p 7002 -s 1
