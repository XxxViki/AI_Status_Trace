@echo off
rem AI Status hook wrapper v5: queue-to-file (#030)
rem Network moved OFF the AI critical path: hook only appends a local file (~30ms);
rem the resident watchdog forwards the queue to the board with retries.
rem %1=event, %2=tool source (claude/zcode)
echo [%date% %time%] ev=%~1 src=%~2 cwd=%cd% >> "%USERPROFILE%\.ai_status\hook_exec.log"
set "QDIR=%USERPROFILE%\.ai_status\queue"
if not exist "%QDIR%" mkdir "%QDIR%"
findstr /R ".*" > "%QDIR%\%~1_%~2_%RANDOM%%RANDOM%.ev"
exit /b 0
