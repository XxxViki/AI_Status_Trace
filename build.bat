@echo off
rem Build firmware: load IDF env (via idf.bat) then build AI_Status
rem First run on a fresh clone: creates sdkconfig from sdkconfig.defaults.
set "MSYSTEM="
set "IDF_GITHUB_ASSETS=dl.espressif.com/github_assets"
cd /d %~dp0
if not exist sdkconfig call idf.bat set-target esp32c3
call idf.bat build
