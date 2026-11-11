@echo off
rem Сборка смоук-теста
setlocal
cd /d "%~dp0"

set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set "VSPATH=%%i"

if "%VSPATH%"=="" (
    echo Не найден VS Build Tools
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cl /nologo /EHsc /std:c++17 /MT /W3 /utf-8 ^
   /I..\..\src /I..\..\include ^
   smoke.cpp ^
   /Fe:smoke.exe || exit /b 1

del smoke.obj
echo Готово: smoke.exe
