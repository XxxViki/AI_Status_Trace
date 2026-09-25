@echo off
rem AI Status hook wrapper v4: self-log + src param + hardcoded curl (#021/#022)
rem %1=event, %2=tool source (claude/zcode)
echo [%date% %time%] ev=%~1 src=%~2 cwd=%cd% >> "%USERPROFILE%\.ai_status\hook_exec.log"
set "EV=%~1"
set "SRC=%~2"
if "%EV%"=="pre-tool-use" goto fast
if "%EV%"=="post-tool-use" goto fast
if "%EV%"=="prompt-submit" goto fast
C:\Windows\System32\curl.exe -s -m 3 --retry 3 --retry-all-errors --retry-delay 1 -X POST "http://192.168.1.20/events?event_type=%EV%&src=%SRC%" --data-binary @- >nul 2>&1
exit /b 0
:fast
C:\Windows\System32\curl.exe -s -m 2 --retry 1 --retry-all-errors -X POST "http://192.168.1.20/events?event_type=%EV%&src=%SRC%" --data-binary @- >nul 2>&1
exit /b 0
