@echo off
rem Flash firmware to the board.
rem Usage:  flash.bat [COM port]     default COM9, e.g.  flash.bat COM5
set "MSYSTEM="
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM9"
cd /d %~dp0
call idf.bat -p %PORT% flash
