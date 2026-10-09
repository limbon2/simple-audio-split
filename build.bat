@echo off
rem Builds Simple Audio Split with the Visual Studio C++ tools (x64).
rem   build\SimpleAudioSplit.exe  the app
rem   build\sas-cli.exe           console diagnostics (list devices, run, verify)
rem   build\dsp_test.exe          offline tests for the signal processing
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || goto novs

if not exist build mkdir build
set CFLAGS=/nologo /std:c++20 /utf-8 /O2 /MT /EHsc /W4 /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\
set LIBS=ole32.lib user32.lib shell32.lib comctl32.lib advapi32.lib avrt.lib

rc /nologo /fo build\app.res res\app.rc || exit /b 1
cl %CFLAGS% /c src\app.cpp src\engine.cpp src\wasapi_util.cpp src\tone.cpp src\settings.cpp src\cli.cpp tests\dsp_test.cpp || exit /b 1
link /nologo /MANIFEST:NO /SUBSYSTEM:WINDOWS /OUT:build\SimpleAudioSplit.exe build\app.obj build\engine.obj build\wasapi_util.obj build\tone.obj build\settings.obj build\app.res %LIBS% || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:build\sas-cli.exe build\cli.obj build\engine.obj build\wasapi_util.obj build\tone.obj %LIBS% || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:build\dsp_test.exe build\dsp_test.obj || exit /b 1
echo.
echo Built build\SimpleAudioSplit.exe
exit /b 0

:novs
echo Visual Studio with the "Desktop development with C++" workload is required.
exit /b 1
