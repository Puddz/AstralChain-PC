"""Builds "Astral Chain Launcher.exe" in the project folder from launcher/Launcher.cs.

  python launcher/build.py

Uses the C# compiler that ships with Windows (.NET Framework 4, C# 5): nothing to install, and the exe runs on any
Windows 10 or 11 as is.
"""
from pathlib import Path
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
OUTPUT = PROJECT/'Astral Chain Launcher.exe'
FRAMEWORK = Path(os.environ.get('WINDIR', r'C:\Windows'))/'Microsoft.NET'/'Framework64'/'v4.0.30319'
REFERENCES = ('System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll',
              'System.Management.dll', 'System.IO.Compression.dll', 'System.IO.Compression.FileSystem.dll')


def main():
    csc = FRAMEWORK/'csc.exe'
    if not csc.is_file():
        sys.exit(f'{csc} not found (the .NET Framework 4 that comes with Windows)')
    command = [str(csc), '/nologo', '/target:winexe', '/optimize+', '/platform:anycpu', f'/out:{OUTPUT}',
               f'/win32manifest:{HERE/"launcher.manifest"}', *(f'/r:{FRAMEWORK/r}' for r in REFERENCES),
               str(HERE/'Launcher.cs')]
    result = subprocess.run(command, capture_output=True, text=True, errors='replace')
    print((result.stdout + result.stderr).strip())
    if result.returncode:
        sys.exit(result.returncode)
    print(f'{OUTPUT.name}: {OUTPUT.stat().st_size} bytes')


if __name__ == '__main__':
    main()
