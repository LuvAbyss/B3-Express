@echo off
setlocal
if not exist "%~dp0launcher\python\pythonw.exe" (
  echo The launcher's Python is missing. Copy the whole folder, including launcher\python.
  pause
  exit /b 1
)
start "" "%~dp0launcher\python\pythonw.exe" -E -s "%~dp0launcher\bayonetta_launcher.py"
