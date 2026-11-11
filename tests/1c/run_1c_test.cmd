@echo off
rem ============================================================
rem Пакетная проверка драйвера в 1С 8.3.24:
rem   1) XML-файлы внешней обработки -> TestDisplayDriver.epf
rem   2) запуск предприятия с /Execute, лог в result.txt
rem ============================================================
setlocal
set "V8BIN=C:\Program Files\1cv8\8.3.24.1808\bin"
set "DIR=C:\Users\admin\Documents\displaynativeapi\tests\1c"

if "%~1"=="convert" goto convert
if "%~1"=="run" goto run
if "%~1"=="createbase" goto createbase

:createbase
if exist "%DIR%\TestBase" rmdir /s /q "%DIR%\TestBase"
"%V8BIN%\1cv8.exe" CREATEINFOBASE "File=%DIR%\TestBase" /Out "%DIR%\createbase.log"
exit /b %errorlevel%

:convert
if exist "%DIR%\TestDisplayDriver.epf" del /q "%DIR%\TestDisplayDriver.epf"
"%V8BIN%\1cv8.exe" DESIGNER /F "%DIR%\TestBase" /N "Тест" /LoadExternalDataProcessorFromFiles "%DIR%\TestDisplayDriver.epf" /Files "%DIR%\TestDisplayDriver" /Out "%DIR%\convert.log" /DumpResult "%DIR%\convert_ret.txt"
exit /b %errorlevel%

:run
if exist "%DIR%\result.txt" del /q "%DIR%\result.txt"
"%V8BIN%\1cv8.exe" ENTERPRISE /F "%DIR%\TestBase" /N "Тест" /Execute "%DIR%\TestDisplayDriver.epf" /Out "%DIR%\enterprise.log" /DumpResult "%DIR%\enterprise_ret.txt"
exit /b %errorlevel%
