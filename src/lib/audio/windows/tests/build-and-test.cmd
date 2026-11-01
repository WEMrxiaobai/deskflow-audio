@echo off
rem SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
setlocal
rem Run from any directory. Uses an existing VS C++ toolchain; downloads nothing.
set "ROOT=%~dp0..\..\..\..\.."
set "OUT=%ROOT%\build\wasapi-backend"
if defined VSCMD_VER goto compiler_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio vswhere.exe not found. Use an x64 Native Tools Command Prompt.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo No installed Visual Studio C++ toolchain found.
  exit /b 1
)
call "%VS%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
:compiler_ready
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /permissive- /Zc:__cplusplus /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE /Fo"%OUT%\\" /Fe"%OUT%\WasapiBackendTests.exe" "%~dp0..\WasapiBackend.cpp" "%~dp0WasapiBackendTests.cpp" /link ole32.lib uuid.lib avrt.lib
if errorlevel 1 exit /b 1
"%OUT%\WasapiBackendTests.exe" %*
exit /b %errorlevel%
