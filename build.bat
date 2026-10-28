@echo off
setlocal
pushd "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio Installer vswhere.exe was not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo ERROR: Visual Studio C++ x86 tools are not installed.
    exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1

if not exist build mkdir build
rc.exe /nologo /fo build\SCCT_OnlineCoop.res SCCT_OnlineCoop.rc
if errorlevel 1 exit /b 1

cl.exe /nologo /O2 /W4 /MD /GS /guard:cf /Fobuild\SCCT_OnlineCoop.obj /Febuild\SCCT_OnlineCoop.exe SCCT_OnlineCoop.c build\SCCT_OnlineCoop.res /link /DYNAMICBASE /NXCOMPAT /guard:cf /OPT:REF /OPT:ICF
if errorlevel 1 exit /b 1

echo.
echo Build succeeded: build\SCCT_OnlineCoop.exe
popd
exit /b 0
