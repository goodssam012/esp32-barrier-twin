@echo off
REM Barrier gate digital twin - local web server
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  where py >nul 2>nul
  if errorlevel 1 (
    echo [ERROR] Python is not installed. Install it from https://www.python.org
    pause
    exit /b 1
  )
  py -3 server.py
) else (
  python server.py
)
REM exit code 3 = server already running, browser opened -> close window
if %errorlevel%==3 exit /b 0
echo.
echo Server stopped.
pause
