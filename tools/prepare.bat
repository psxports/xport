@echo off
setlocal
python -B "%~dp0duckstation_build.py" %*
exit /b %errorlevel%
