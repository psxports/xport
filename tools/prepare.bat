@echo off
setlocal
python -B "%~dp0adpcm-xq\build.py"
if errorlevel 1 exit /b %errorlevel%
python -B "%~dp0duckstation_build.py" %*
exit /b %errorlevel%
