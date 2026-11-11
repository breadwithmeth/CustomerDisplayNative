@echo off
rem ============================================================
rem Сборка драйвера дисплея покупателя (x64 + x86, Release)
rem Требуются VS 2022 Build Tools с рабочей нагрузкой C++.
rem ============================================================
setlocal
cd /d "%~dp0"

set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -latest -requires Microsoft.Component.MSBuild -find MSBuild\Current\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
)

if "%MSBUILD%"=="" if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
)

if "%MSBUILD%"=="" (
    echo MSBuild не найден. Установите VS 2022 Build Tools с рабочей нагрузкой C++.
    exit /b 1
)

echo MSBuild: %MSBUILD%
echo.

set "SLN=%~dp0CustomerDisplayNative.sln"

echo === Сборка x64 (Release) ===
"%MSBUILD%" "%SLN%" /p:Configuration=Release /p:Platform=x64 /m /v:m || goto :fail
echo.
echo === Сборка x86 (Release) ===
"%MSBUILD%" "%SLN%" /p:Configuration=Release /p:Platform=x86 /m /v:m || goto :fail

echo.
echo Готово. Артефакты:
dir /b bin\Release\*.dll
exit /b 0

:fail
echo СБОРКА ЗАВЕРШИЛАСЬ С ОШИБКОЙ
exit /b 1
