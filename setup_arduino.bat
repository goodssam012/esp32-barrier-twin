@echo off
REM Installs arduino-cli + ESP32 board package + WebSockets library (one time, large download)
REM Needed only to compile .ino sources from the settings window.
REM Board packages are kept in %USERPROFILE%\.arduino15 (same folder server.py uses).
cd /d "%~dp0"
set ARDUINO_DIRECTORIES_DATA=%USERPROFILE%\.arduino15
set ARDUINO_DIRECTORIES_DOWNLOADS=%USERPROFILE%\.arduino15\staging
set ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=https://espressif.github.io/arduino-esp32/package_esp32_index.json
set CLI=arduino-cli
where arduino-cli >nul 2>nul
if errorlevel 1 (
  if exist "%ProgramFiles%\Arduino CLI\arduino-cli.exe" (
    set CLI="%ProgramFiles%\Arduino CLI\arduino-cli.exe"
  ) else (
    echo [1/4] Installing arduino-cli with winget...
    winget install --id ArduinoSA.CLI -e --accept-source-agreements --accept-package-agreements
    set CLI="%ProgramFiles%\Arduino CLI\arduino-cli.exe"
  )
)
echo [2/4] Updating board index...
%CLI% core update-index
echo [3/4] Installing ESP32 board package...
%CLI% core install esp32:esp32
echo [4/4] Installing libraries...
%CLI% lib install WebSockets
echo.
echo Done. Run start.bat and use the settings (gear) button to upload.
pause
