# Runs a built native trace.exe under a GPU-fault watchdog (development builds; the portable bundle has Run.ps1).
#   -Run       the output folder of a native_trace --build-only build (contains round-00\trace.exe)
#   -Seconds   hard time limit
#   -Script    AC_INPUT_SCRIPT (scripted buttons, see runtime/hid_service.h)
#   -Env       extra variables, "NAME=value;NAME=value"
# The game is killed on the first nvlddmkm (NVIDIA), amdkmdag (AMD) or Display 4101 (TDR recovered) event logged after
# its start. Polls every 250 ms; run.txt records the reason. Exit code: the game's, or 90 watchdog, 91 time limit.
param([Parameter(Mandatory=$true)][string]$Run,[int]$Seconds=300,[string]$Script='',[string]$Env='')
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$run=(Resolve-Path $Run).Path
$round=Join-Path $run 'round-00'
$exe=Join-Path $round 'trace.exe'
if(!(Test-Path $exe)){throw "no build at $exe"}
$log=Join-Path $round 'trace.log'; $err=Join-Path $round 'stderr.log'; $notes=Join-Path $run 'run.txt'

$vars=@{
  AC_NVN_HLE_BOOTSTRAP='0x700d110360'; AC_NVN_HLE_LINKED='vulkan'; AC_IGNORE_FOCUS='1'
  AC_GLSLANG=(Join-Path $project 'tools\glslang\bin\glslang.exe')
  AC_HLE_SYMBOLS='_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE=0x700d00aa70;_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE=0x700d00a560;_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE=0x700d00a700'
}
# Builds record their own SDK-derived addresses (native_trace --build-only writes run-env.txt).
$runEnv=Join-Path $run 'run-env.txt'
if(Test-Path $runEnv){ foreach($line in Get-Content $runEnv){ if($line -match '^([^=]+)=(.*)$'){$vars[$Matches[1]]=$Matches[2]} } }
if($Script){$vars.AC_INPUT_SCRIPT=$Script}
foreach($pair in ($Env -split ';')){ if($pair -match '^([^=]+)=(.*)$'){$vars[$Matches[1]]=$Matches[2]} }
foreach($k in $vars.Keys){[Environment]::SetEnvironmentVariable($k,$vars[$k],'Process')}

$gpu=(Get-CimInstance Win32_VideoController | ForEach-Object {"$($_.Name) driver $($_.DriverVersion)"}) -join '; '
"start $(Get-Date -Format s)`ngpu $gpu`nlimit ${Seconds}s`n" + (($vars.Keys | Sort-Object | ForEach-Object {"$_=$($vars[$_])"}) -join "`n") |
  Out-File -Encoding utf8 $notes

$start=Get-Date
$game=Start-Process -FilePath $exe -WorkingDirectory $run -ArgumentList ('"'+(Join-Path $round 'relocations.bin')+'"') `
  -RedirectStandardOutput $log -RedirectStandardError $err -PassThru -WindowStyle Normal
$reason=''
$filter=@{LogName='System';StartTime=$start}
while(-not $game.HasExited){
  Start-Sleep -Milliseconds 250
  $events=@(Get-WinEvent -FilterHashtable $filter -MaxEvents 50 -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -in 'nvlddmkm','amdkmdag','amdkmdap' -or ($_.ProviderName -eq 'Display' -and $_.Id -eq 4101) })
  if($events.Count){
    Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue
    $e=$events[-1]
    $reason="WATCHDOG $(Get-Date -Format s): $($events.Count) driver event(s); first: $($e.ProviderName) id $($e.Id) $($e.Message)"
    break
  }
  if(((Get-Date)-$start).TotalSeconds -ge $Seconds){
    Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue
    $reason="TIME LIMIT ${Seconds}s"
    break
  }
}
$game.WaitForExit()
$code=$game.ExitCode
if($reason -like 'WATCHDOG*'){$code=90}elseif($reason){$code=91}
Add-Content -Encoding utf8 $notes "$reason`nend $(Get-Date -Format s) exit=$($game.ExitCode)"
Write-Output "$reason end exit=$($game.ExitCode)"
exit $code
