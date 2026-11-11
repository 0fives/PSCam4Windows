@echo off
rem Builds build\PSCam4WinDS32.dll: the 32-bit DirectShow camera filter, for 32-bit
rem applications that cannot load the x64 PSCam4Win.dll. Run by build.bat in its
rem own cmd.exe, because the x86 toolchain environment cannot share a shell with
rem the x64 one.
rem
rem   build_ds32.bat <vcvars32.bat> <repo root with trailing \> <output dir>
setlocal
call "%~1" >nul
if errorlevel 1 (
    echo error: could not initialize the Visual Studio x86 toolchain ^(vcvars32.bat^)
    exit /b 1
)
set "ROOT=%~2"
set "OUT=%~3"
if not exist "%OUT%\x86" mkdir "%OUT%\x86"

set CFLAGS=/nologo /c /O2 /MT /EHsc /std:c++17 /utf-8 /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS

cl %CFLAGS% "%ROOT%source\DsCamera.cpp" /Fo"%OUT%\x86\DsCamera.obj" || exit /b 1
cl %CFLAGS% "%ROOT%source\DsOnly32.cpp" /Fo"%OUT%\x86\DsOnly32.obj" || exit /b 1
link /nologo /DLL /MACHINE:X86 /DEF:"%ROOT%source\PSCam4WinDS32.def" /OUT:"%OUT%\PSCam4WinDS32.dll" ^
    "%OUT%\x86\DsCamera.obj" "%OUT%\x86\DsOnly32.obj" ^
    strmiids.lib ole32.lib advapi32.lib
if errorlevel 1 exit /b 1
endlocal
