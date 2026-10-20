@echo off
rem Astral Chain PC with frame diagnostics: frames over 50 ms are logged with where their time went ([hitch] lines in
rem build\game\round-00\stderr.log). Costs a little performance; use it to send a report, then go back to the normal
rem launcher.
set LARECOMP_VK_PROF=1
call "%~dp0Play Astral Chain.cmd"
