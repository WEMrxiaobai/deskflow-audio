# 双机主功能开发预览

按用户“先跑主功能”的要求，暂停非必要的矩阵扩展/边界优化，先提供Windows x64单源系统声音发送与接收程序。它不改键鼠协议，不装服务，也不是原计划的最终统一配对。

## Windows 安装包

[Windows x64 MSI安装包](../../build/audio/Deskflow-Audio-Preview-0.1.0-x64.msi)可双击安装，包含中文向导、当前用户安装目录、开始菜单发送/接收/说明入口、卸载项和app-local VC运行库。不安装服务、不自动采集、不改防火墙、不设开机启动。尚未代码签名，Windows可能显示发布者未知。安装后通过开始菜单“Deskflow 音频共享预览”使用。

[构建脚本](../../tools/audio/build_preview_installer.py)采用WiX3.14.1；当前构建机Windows Installer服务不可用，ICE校验因服务失败，显式`--skip-ice`生成；未完成实际安装/卸载验收。已使用原生MSI数据库API确认22个Dialog、215个Control、17个UI sequence项、18个文件及4个快捷方式，并解包核验exe哈希与已测试预览一致。该验证不等于实际安装通过。

## 直接体验

[预览压缩包](../../build/audio/Deskflow音频共享预览.zip)解压后把整个目录复制到两台Windows电脑。先在接收电脑运行[接收入口](../../src/apps/deskflow-audio-preview/接收远端声音.cmd)，确认播放授权并保存新临时邀请，通过可信渠道交给发送电脑。发送电脑播放音乐，运行[发送入口](../../src/apps/deskflow-audio-preview/发送系统声音.cmd)，确认采集授权并打开邀请。初始播放增益35%；不要为试听调高系统音量。Ctrl+C停止。

临时邀请10分钟失效；不填写IP/端口/密钥，自动包含接收机IPv4地址。仅同局域网，不承诺NAT/公网穿透。Windows可能提示允许程序通过**专用网络**；程序不自动修改防火墙或系统主音量。完整限制见[使用说明](../../src/apps/deskflow-audio-preview/使用说明.txt)。

## 必要安全边界

- 邀请是明文、短时bearer授权，不是经过身份绑定的Deskflow统一配对；仅通过可信方式给指定对端。用户有明确发送/接收确认。原计划“无音频二次配对”仍未完成，不能将此临时入口冒充最终交付。
- 邀请CREATE_NEW禁止覆盖、创建时owner-only protected DACL；严格大小/IPv4/格式/期限验证，读取拒绝末级reparse。复制后文件权限由目标路径决定。
- 随机会话nonce经HMAC派生独立SRTP主密钥/盐，同一短时邀请重开不能复用旧keystream。HMAC握手验证持有授权，接收仅首个有效HELLO并固定endpoint+nonce，重复同HELLO仅ACK，其他来源不能触发格式/媒体重建。STOP也验证HMAC/endpoint/nonce。
- 最长运行至邀请到期，重新开始必须新邀请；接收退出或设备变化不默默接纳新会话。Ctrl+C协作停设备回调后停encoder，socket晚于worker销毁。单Windows会话禁止同时运行两个非测试预览，降低系统回环回传风险。
- 不写PCM或密钥日志；临时秘密在内存结束时清除。邀请副本需要用户自己删除，不承诺远程安全擦除。HMAC不证明对端身份，也不对同账号恶意进程保密。

## 已验证与未验证

- Release预览成功构建，邀请回归覆盖文件/DACL、包篡改、过期输入和nonce派生；[测试源码](../../src/apps/deskflow-audio-preview/InvitationTests.cpp)。
- 两个独立进程，真实邀请/HMAC握手、UDP/SRTP、Opus，发送synthetic PCM、接收解码丢弃，5秒测试两进程exit0：[发送日志](../../build/audio/preview-sender.txt)、[接收日志](../../build/audio/preview-receiver.txt)。该模式不打开声卡，不代表跨机音质/声卡验收。
- 新预览另通过`--test-render`打开真实默认声卡，另一个进程发送5秒低音量synthetic PCM，两个进程exit0：[声卡接收日志](../../build/audio/preview-receiver-render.txt)。该模式不再由worker丢弃PCM；程序要求至少100次设备render callback且至少600包解码。日志同时报告启动欠载，不将其冒充全程无损、实际听感或跨机物理延迟测量。
- 发送采集闭环通过`--test-capture`：独立probe先用默认声卡播放低音量测试音，新预览用WASAPI loopback采集、编码、SRTP发送，另一个新预览进程仅解码丢弃，避免本机回传。5秒测试exit0，486次真实capture callback、972包发送、969包解码、零jitter loss、解码峰值0.0107426（非静音）。接收7952帧欠载主要是接收先启动、发送设备随后打开，首秒后未增加；不是全程无损。见[采集发送日志](../../build/audio/preview-sender-capture.txt)和[采集接收日志](../../build/audio/preview-receiver-capture.txt)。这和声卡输出测试一起覆盖本机两端主链路，但不等于两台物理电脑已试听。
- 正常及C++异常退出都在设备/encoder停止后通过作用域守卫发送三次HMAC STOP（best effort，不保证UDP送达），减少单端报错后另一端继续传输。强杀/崩溃仍只依赖到期，自动重连延后。
- WASAPI捕获/播放沿用已验证后端；当前默认设备变化、>10ms callback、持续无capture callback后的媒体重启仍要求重新启动。自动恢复、多来源、音量界面、统一授权、持久化、安装均延后。
- 真实两机音乐试听、端到端延迟和长期稳定性未测；不得宣称20–40ms、三机主开关、生产发布完成。

## 构建入口

[preview源代码](../../src/apps/deskflow-audio-preview/main.cpp)由[音频CMake](../../src/lib/audio/CMakeLists.txt)的开发probe选项构建。目标`deskflow-audio-preview`，邀请测试`audio-preview-invitation-tests`。默认主产品开关仍OFF。预览包带许可证及当前音频对应源码/固定依赖源码，非正式Deskflow安装包。
