@echo off
chcp 65001 >nul
setlocal
pushd "%~dp0"
echo Deskflow 音频主链路试听（开发预览）
echo.
echo 将播放 10 秒低音量测试音，请先调低耳机音量。
echo 链路：测试音 - Opus - 本机加密 UDP - 解码 - 默认声卡。
echo 这不是跨机系统音频共享；不会录音，不修改系统音量。
echo.
if not exist "build\audio\Release\deskflow-audio-probe.exe" (
  echo 未找到已构建的演示程序，请先构建音频原型。
  popd
  pause
  exit /b 1
)
"build\audio\Release\deskflow-audio-probe.exe" --render-probe 10 5
set "result=%errorlevel%"
echo.
if not "%result%"=="0" echo 演示未成功，请把上面的错误信息发给开发者。
popd
pause
exit /b %result%
