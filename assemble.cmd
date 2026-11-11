@echo off
rem ============================================================
rem Упаковка внешней компоненты в zip для ПодключитьВнешнююКомпоненту
rem Результат: CustomerDisplayNative.zip (MANIFEST.xml + DLL)
rem ============================================================
setlocal
cd /d "%~dp0"

if not exist bin\Release\CustomerDisplayNative_64.dll (
    echo Сначала выполните сборку: build.cmd
    exit /b 1
)

set "STAGE=%TEMP%\CustomerDisplayNative_pack"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"

copy /y MANIFEST.xml "%STAGE%\" >nul
copy /y INFO.XML "%STAGE%\" >nul
copy /y bin\Release\CustomerDisplayNative_32.dll "%STAGE%\" >nul
copy /y bin\Release\CustomerDisplayNative_64.dll "%STAGE%\" >nul

if exist CustomerDisplayNative.zip del /q CustomerDisplayNative.zip

powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath 'CustomerDisplayNative.zip'" || exit /b 1

echo Готово: CustomerDisplayNative.zip
dir CustomerDisplayNative.zip | find "CustomerDisplayNative.zip"
endlocal
