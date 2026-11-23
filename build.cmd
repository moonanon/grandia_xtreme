@echo off
rem Build the port in one step (port/tools/build_all.py): double-click, or
rem build.cmd [--disc IMAGE] [--32 | --both]
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 port\tools\build_all.py %*
) else (
    where python >nul 2>nul
    if errorlevel 1 (
        echo Python 3 is needed: https://www.python.org/downloads/
        pause
        exit /b 1
    )
    python port\tools\build_all.py %*
)
set rc=%errorlevel%
rem Started without options (a double click): keep the window open
if "%~1"=="" pause
exit /b %rc%
