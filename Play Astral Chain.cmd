@echo off
rem Astral Chain PC: starts the game (60 fps, guest threads in parallel) under the GPU driver watchdog.
rem After an update of this project the game is rebuilt once first (about a minute).
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (echo Run setup.cmd first. & pause & exit /b 1)
".venv\Scripts\python.exe" -m astral_chain.setup_game --rebuild-if-stale || (pause & exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File astral_chain\run_guarded.ps1 -Run build\game -Seconds 604800 -Env "AC_PARALLEL=1;AC_IGNORE_FOCUS="
if errorlevel 1 pause
