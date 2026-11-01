@echo off
rem SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
setlocal
set "ROOT=%~dp0..\..\..\..\.."
set "OUT=%ROOT%\build\endpoint-mute-guard"
if defined VSCMD_VER goto compiler_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS exit /b 1
call "%VS%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
:compiler_ready
if not exist "%OUT%" mkdir "%OUT%"
set "FLAGS=/nologo /std:c++20 /EHsc /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE"
cl %FLAGS% /Fo"%OUT%\\" /Fe"%OUT%\EndpointMuteGuardTests.exe" "%~dp0..\EndpointMuteGuard.cpp" "%~dp0EndpointMuteGuardTests.cpp" /link ole32.lib uuid.lib
if errorlevel 1 exit /b 1
"%OUT%\EndpointMuteGuardTests.exe"
if errorlevel 1 exit /b 1
cl %FLAGS% /Fo"%OUT%\\" /Fe"%OUT%\deskflow-audio-mute-watchdog.exe" "%~dp0..\EndpointMuteGuard.cpp" "%~dp0..\MuteWatchdogMain.cpp" /link ole32.lib uuid.lib
if errorlevel 1 exit /b 1
cl %FLAGS% /c /Fo"%OUT%\\" "%~dp0..\MuteWatchdog.cpp"
exit /b %errorlevel%
