# Astral Chain PC: the Python that setup runs on. Downloads a pinned CPython (python.org's build, as published on
# NuGet; SHA-256 checked) into tools\python and creates .venv from it, so no Python install is needed and the
# installed one, if any, does not matter. An existing .venv is kept if it already has the setup packages.
# Used by setup.cmd and the launcher; the packages themselves are installed by them (requirements.txt).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # (Windows PowerShell's progress bar slows downloads a lot)
$project = Split-Path -Parent $PSScriptRoot
$version = '3.14.9'
$url = "https://api.nuget.org/v3-flatcontainer/python/$version/python.$version.nupkg"
$sha256 = '7cd9d07aec975212344dc6f7e45019378cf6775dd564181e269010a1eddaaec7'
$tools = Join-Path $project 'tools'
$python = Join-Path $tools 'python'
$venv = Join-Path $project '.venv'
$venvPython = Join-Path $venv 'Scripts\python.exe'

try {
    $bundled = [IO.Path]::GetFullPath($python)
    if (Test-Path $venvPython) {
        $cfg = Get-Content (Join-Path $venv 'pyvenv.cfg') -ErrorAction SilentlyContinue
        $ours = $cfg | Where-Object { $_ -match '^home\s*=\s*(.*)$' -and $Matches[1].Trim() -eq $bundled }
        if ($ours -and (Test-Path (Join-Path $python 'python.exe'))) { exit 0 }
        # Made from another Python (older setups): keep it if it works, else start again from the downloaded one
        $ErrorActionPreference = 'Continue'  # (its error output must not stop this script)
        & $venvPython -c 'import capstone, lz4' 2>$null
        $works = $LASTEXITCODE -eq 0
        $ErrorActionPreference = 'Stop'
        if ($works) { exit 0 }
        Write-Output '== The Python environment (.venv) does not work: creating it again'
        Remove-Item $venv -Recurse -Force
    }

    if (-not (Test-Path (Join-Path $python 'python.exe'))) {
        Write-Output "== Downloading Python $version"
        New-Item -ItemType Directory -Force $tools | Out-Null
        $archive = Join-Path $tools 'python.zip'
        $partial = Join-Path $tools 'python.partial'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest $url -OutFile $archive -UseBasicParsing
        if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $sha256) {
            Remove-Item $archive
            throw "Python: the download does not match its pinned SHA-256 ($url)"
        }
        if (Test-Path $partial) { Remove-Item $partial -Recurse -Force }
        Expand-Archive $archive $partial
        Move-Item (Join-Path $partial 'tools') $python
        Remove-Item $partial -Recurse -Force
        Remove-Item $archive
    }

    Write-Output '== Creating the Python environment'
    & (Join-Path $python 'python.exe') -m venv $venv
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $venvPython)) { throw 'Could not create the Python environment (.venv)' }
} catch {
    Write-Output "Setup stopped: $($_.Exception.Message)"
    exit 1
}
