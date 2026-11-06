@echo off
setlocal
if not exist "%~dp0launcher\python\pythonw.exe" (
  echo The launcher's Python is missing. Copy the whole folder, including launcher\python.
  pause
  exit /b 1
)
rem the new window (web_launcher.py); where WebView2 is missing it opens the old one by itself
start "" "%~dp0launcher\python\pythonw.exe" -E -s "%~dp0launcher\web_launcher.py"
