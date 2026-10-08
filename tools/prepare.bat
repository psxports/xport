@echo off
setlocal
if /I "%~1"=="--web" goto web
python -B "%~dp0toolchain_preflight.py"
if errorlevel 1 exit /b %errorlevel%
if /I "%~1"=="--verify-only" goto duckstation
python -B "%~dp0adpcm-xq\build.py"
if errorlevel 1 exit /b %errorlevel%
:duckstation
python -B "%~dp0duckstation_build.py" %*
exit /b %errorlevel%

:web
if not exist "%~dp0emsdk\emsdk.bat" git clone --depth 1 https://github.com/emscripten-core/emsdk.git "%~dp0emsdk"
if errorlevel 1 exit /b %errorlevel%
call "%~dp0emsdk\emsdk.bat" install 6.0.5
if errorlevel 1 exit /b %errorlevel%
call "%~dp0emsdk\emsdk.bat" activate 6.0.5
exit /b %errorlevel%
