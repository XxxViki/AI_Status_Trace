@echo off
rem ESP-IDF entry for Git-Bash/ZCode: clear MSYSTEM then load IDF env
rem Usage:  idf.bat <any idf.py args>      e.g.  idf.bat build
rem IDF location: %IDF_PATH% if already set, else the default install path below.
set "MSYSTEM="
if not defined IDF_PATH set "IDF_PATH=C:\esp\v6.1\esp-idf"
if not exist "%IDF_PATH%\export.bat" (
    echo [idf.bat] ESP-IDF export.bat not found under "%IDF_PATH%"
    echo [idf.bat] Set IDF_PATH to your esp-idf folder, or edit this line, then retry.
    exit /b 1
)
call "%IDF_PATH%\export.bat"
idf.py %*
