@echo off
rem Astral Chain PC: one-time setup. Builds the native game from your own dump and keys (see README.md).
rem Python is downloaded into tools\python (pinned, SHA-256 checked); an installed Python is not needed.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File astral_chain\get_python.ps1 || (pause & exit /b 1)
".venv\Scripts\python.exe" -m pip install --disable-pip-version-check -q -r requirements.txt || (echo Could not install the Python packages. & pause & exit /b 1)
".venv\Scripts\python.exe" -m astral_chain.setup_game %*
pause
