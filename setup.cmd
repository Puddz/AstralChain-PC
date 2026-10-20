@echo off
rem Astral Chain PC: one-time setup. Builds the native game from your own dump and keys (see README.md).
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  set PY=
  for %%V in (3.14 3.13 3.12) do if not defined PY py -%%V -c "" >nul 2>&1 && set PY=py -%%V
  if not defined PY (
    echo Python 3.12 or newer is required: https://www.python.org/downloads/windows/
    echo Tick "Add python.exe to PATH" / keep the "py launcher" option in the installer, then run this again.
    pause
    exit /b 1
  )
  echo Creating the Python environment...
  call %%PY%% -m venv .venv || (echo Could not create .venv & pause & exit /b 1)
)
".venv\Scripts\python.exe" -m pip install --disable-pip-version-check -q -r requirements.txt || (echo Could not install the Python packages. & pause & exit /b 1)
".venv\Scripts\python.exe" -m astral_chain.setup_game %*
pause
