@echo off
chcp 65001 >nul
pushd "%~dp0"
echo 发送端：先播放音乐，确认提示后选择接收端给你的邀请文件。
echo 当前默认声卡的全部系统声音会发送到指定电脑，不采集麦克风。
echo 按 Ctrl+C 停止；不要同时运行接收端，以免回传。
"deskflow-audio-preview.exe" --send
echo.
pause
popd
