@echo off
chcp 65001 >nul
pushd "%~dp0"
echo 接收端：先确认提示，然后选择新邀请文件的保存位置。
echo 把邀请复制到发送电脑；邀请含临时密钥，请勿公开发送。
echo 请先降低耳机音量；按 Ctrl+C 停止。
"deskflow-audio-preview.exe" --receive
echo.
pause
popd
