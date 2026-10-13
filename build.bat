@echo off
rem Builds build\HogsFix.asi (x86, no CRT). Needs Visual Studio 2019 or newer with "Desktop development with C++".
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - install Visual Studio & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo no Visual Studio with C++ tools found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cd build
cl /nologo /c /O2 /GS- /Zl /W3 /arch:SSE2 /fp:fast ..\src\hogsfix.c ..\src\battle.c ..\src\display.c ..\src\cdaudio.c ..\src\gamepad.c || exit /b 1
link /nologo /DLL /NODEFAULTLIB /ENTRY:DllMain /DEF:..\src\hogsfix.def /OUT:HogsFix.asi hogsfix.obj battle.obj display.obj cdaudio.obj gamepad.obj kernel32.lib user32.lib winmm.lib || exit /b 1
echo built build\HogsFix.asi
