# 音频共享 P0 源码审查、参考固定与 P1 关口

> 历史审查记录；当前三机双来源功能范围与剩余验收见[集成说明](audio-sharing-integrated.md)。

## 1. 范围、基线与结论

- 本文保留P0只读审查时的基线和结论，不是实施计划替代品。下文“未实现/未测试/不改构建”指该审查子任务当时的范围，并非当前代码状态；后续音频实现、依赖固定、构建及测试证据统一见[实施记录](audio-sharing-implementation.md)和[验证手册](audio-sharing-p1-validation.md)。原计划、现有协议及IPC未被改写。
- Deskflow 审查基线：[`2877600facc0cf1b38b7f50b51cdc8fe20052bf2`](https://github.com/deskflow/deskflow/commit/2877600facc0cf1b38b7f50b51cdc8fe20052bf2)。开始审查时工作区另有未跟踪计划及音频目录；它们不属于本次已完成审查的产品实现证据。
- RustDesk 唯一固定参考：[`23e5bb471a040d62f0a6ad52591fb30784265b73`](https://github.com/rustdesk/rustdesk/commit/23e5bb471a040d62f0a6ad52591fb30784265b73)，不以 master、当前网页版本或浮动分支内容代替。
- **本次实现路线选择：直接 C++ / Windows WASAPI + 独立来源的 libopus。CPAL 不作为依赖，也不经 Rust/FFI 间接引入。** 这是架构及依赖边界决定，不表示采集、编码、传输、播放已经实现或验证。
- **P0 文档审查完成；P1 仍是必须验证的关口，不是已通过的验收。P2–P5 未在本文完成、测试或认证。** 本次没有执行音频端到端、互操作、权限隔离、安装包或设备测试。
- **自动音频启用必须 fail-closed。** 现有键鼠连接成功、电脑名白名单、TLS 开关开启、IPC hello 成功，任一单独条件都不足以授权音频采集/发送。未满足第 7 节全部前提时保持关闭，不静默降级到不安全模式。

## 2. 本仓库安全身份：加密、信任与授权不能混为一谈

### 2.1 实际安全级别与证书行为

源码证据：

- [SecurityLevel.h:13–18](../../src/lib/net/SecurityLevel.h#L13-L18) 区分 PlainText、Encrypted、PeerAuth。
- [ServerApp.cpp:443–455](../../src/lib/deskflow/ServerApp.cpp#L443-L455) 显示：TLS 关闭进入 PlainText；TLS 开启但关闭客户端指纹检查进入 Encrypted；只有同时开启检查才进入 PeerAuth。
- [TCPSocketFactory.cpp:26–44](../../src/lib/net/TCPSocketFactory.cpp#L26-L44) 确实保留明文 socket 路径。
- [SecureSocket.cpp:329–368](../../src/lib/net/SecureSocket.cpp#L329-L368) 禁止 TLS 1.2 之前版本；PeerAuth 请求对端证书，但证书链校验回调不等同于系统 CA/主机名验证，后续依靠指纹信任。
- [SecureSocket.cpp:431–438](../../src/lib/net/SecureSocket.cpp#L431-L438) 服务端只在 PeerAuth 下验证客户端证书指纹；[SecureSocket.cpp:493–505](../../src/lib/net/SecureSocket.cpp#L493-L505) 客户端校验服务端指纹。
- [SecureSocket.cpp:616–653](../../src/lib/net/SecureSocket.cpp#L616-L653) 计算 SHA-256 并查找 trusted 数据库；未信任、空库/读失败等路径不会自动接纳未知指纹。
- [MainWindow.cpp:844–885](../../src/lib/gui/MainWindow.cpp#L844-L885) 用户确认指纹后才写入数据库。这个“信任此证书”操作没有表达“允许分享系统声音”。

**结论：** 不应把 Encrypted 解释为双向身份认证，也不能把 OpenSSL 握手成功解释为已取得音频权限。音频进入就绪态必须等到指纹校验及应用层握手最终成功；不要读取握手中间的 ready 标志提前开采集。首次指纹需通过可靠带外方式核对；普通确认弹窗不解决错误信任/社会工程风险。

### 2.2 电脑名不是音频接收者身份

[ClientProxyUnknown.cpp:197–222](../../src/lib/server/ClientProxyUnknown.cpp#L197-L222) 从 hello 解析客户端自报名称。[Server.cpp:245–267](../../src/lib/server/Server.cpp#L245-L267) 检查名称存在于配置且不重复，但这不是名称与证书的绑定。[FingerprintDatabase.cpp:76–86](../../src/lib/net/FingerprintDatabase.cpp#L76-L86) 只存储/比较指纹，未把该指纹限定到某个屏幕名。

因此，即使对端证书已在全局信任集合中，也不能据此证明它有权冒用另一配置名称获得该名称的音频。**音频授权应绑定实际验证过的证书 SHA-256、当前连接/会话代次、方向和明确用户许可；电脑名只作显示/路由标签。** 证书轮换、换名、重连、控制目标变化不能继承未经复核的旧授权；同名不等于同设备，同证书也不等于同一个 Windows 登录用户。

### 2.3 配置、私钥和登录会话仍是待验证的边界

[Settings.cpp:446–468](../../src/lib/common/Settings.cpp#L446-L468) 显示 Windows 非便携模式使用系统配置位置，TLS 数据库位于其 tls 目录，便携模式路径不同。源码路径选择不证明安装后的 ACL、私钥保护或当前用户所有权成立；本文没有审计真实机器安装 ACL、证书文件读取者、配置替换竞争或便携目录信任。**不可将机器级信任列表视为当前桌面用户对音频的同意。** 许可的持久化文件必须按 SID/会话与设备身份隔离并保护写权限。

## 3. 旧协议与未知消息兼容性

### 3.1 不能将“有长度前缀”推导为“旧端安全跳过新消息”

- [PacketStreamFilter.cpp:33–65](../../src/lib/deskflow/PacketStreamFilter.cpp#L33-L65) 在读完包后可继续进入下一包；[69–80](../../src/lib/deskflow/PacketStreamFilter.cpp#L69-L80) 每次写入使用 4 字节长度前缀；[108–120](../../src/lib/deskflow/PacketStreamFilter.cpp#L108-L120) 有协议总消息长度限制。
- 但客户端 [ServerProxy.cpp:79–119](../../src/lib/client/ServerProxy.cpp#L79-L119) 遇到未知 4 字节码时循环读弃直到没有现成数据。服务端 [ClientProxy1_0.cpp:108–142](../../src/lib/server/ClientProxy1_0.cpp#L108-L142) 同样如此。
- 这个行为不是“按扩展帧长度只跳过一个未知消息”；可能顺带丢弃当前已缓冲的后续合法消息，导致键鼠、剪贴板或心跳异常。不完整消息码/解析异常还有断连路径。
- [ClientProxyUnknown.cpp:140–188](../../src/lib/server/ClientProxyUnknown.cpp#L140-L188) 明确列出 1.0–1.8 代理，不支持的版本拒绝；不能仅增加 minor 版本并假设双方自动协商新特性。

**P1 兼容性门禁：** 未获得可验证的双方能力确认前，不向旧协议流发送任何新音频码、探测载荷或假借既有码封装的扩展。不能为了测试能力先发一个未知码；那本身就可能破坏旧端。需要证明协商入口在所有目标旧端安全，或选择经过审查的带外能力/独立通道机制；本文不替现有计划擅定协议方案。

### 3.2 音频数据面的要求（尚未实现/验收）

- 以已认证控制连接派生授权；如开独立通道，须绑定证书身份、连接代次、一次性会话凭据及方向，不能因来自同 IP/固定端口就接纳音频。
- 协商格式与协议版本；限制消息字节数、解码后帧数、采样率、声道、帧时长、序列和会话代次；溢出计算用有界验证，未就绪/错误格式帧拒绝。
- 控制面、音频队列和解码输出均有独立上限；旧流的总消息上限不是音频延迟/内存预算。不能让音频占满键鼠线程、TLS 写队列或事件队列。
- 断连、拒绝、锁屏、注销、会话切换及身份变化应立即停止发送并清空旧代次数据；超时不能解释为同意。
- 后续需新旧两向互通、同包/多包相邻消息、半包/粘包、畸形长度和重连/重放负向测试。本文没有以源码阅读代替这些测试。

## 4. IPC 用户边界：当前 IPC 不可直接承载音频授权

### 4.1 已确认的静态行为

- [IpcServer.cpp:31–39](../../src/lib/deskflow/ipc/IpcServer.cpp#L31-L39) 为系统进程与普通 GUI 通信显式使用 `WorldAccessOption`。
- [Constants.h.in:13–24](../../src/lib/common/Constants.h.in#L13-L24) 使用固定 core/daemon 名称，不含用户 SID 或会话标识。
- [IpcServer.cpp:119–157](../../src/lib/deskflow/ipc/IpcServer.cpp#L119-L157) 的 hello 只比较版本与 Git SHA；没有每个 socket 的已认证用户状态，其他命令可以直接进入 processCommand，并不要求先完成 hello。
- [CoreIpcServer.cpp:30–40](../../src/lib/deskflow/ipc/CoreIpcServer.cpp#L30-L40) 接受 stop；[DaemonIpcServer.cpp:26–89](../../src/lib/deskflow/ipc/DaemonIpcServer.cpp#L26-L89) 接受 start、stop、configFile、clearSettings 等命令。已审查路径未见调用者 SID/登录 SessionId 校验。
- [IpcServer.cpp:145–151、162–180](../../src/lib/deskflow/ipc/IpcServer.cpp#L145-L180) 有待发事件重放及向所有连接广播，不能把某个收到通知的 GUI 当作唯一授权用户。
- [IpcServer.cpp:66–85](../../src/lib/deskflow/ipc/IpcServer.cpp#L66-L85) 服务端没有完整的跨读取分片累积，readAll 及消息列表也不能当作已经有音频级限流保护。不要通过此文本 IPC 传 PCM 或网络音频帧。
- GUI [IpcClient.cpp:167–175](../../src/lib/gui/ipc/IpcClient.cpp#L167-L175) 在 versionMismatch 分支也会进入 Connected；Connected 是连通性状态，不是身份/兼容/许可证明。

这些是静态源代码风险，不是本次已经复现跨用户漏洞或提权成功的声明。其后果是：**不得添加一个无用户校验的 enableAudio 文本命令，并把它视为用户同意。** IPC 名称、版本字符串、进程存在、GUI 可见与管理员权限均不是授权凭据。

### 4.2 Windows 服务与当前用户不是同一安全主体

[DaemonApp.cpp:65–96](../../src/apps/deskflow-daemon/DaemonApp.cpp#L65-L96) 已明确考虑任意本地用户可达 IPC 的远程配置路径风险；拒绝 UNC 路径是有益现有保护，但不解决音频许可隔离。[MSWindowsWatchdog.cpp:134–165](../../src/lib/platform/MSWindowsWatchdog.cpp#L134-L165) 的 elevated 路径可使用 winlogon 令牌；[263–295](../../src/lib/platform/MSWindowsWatchdog.cpp#L263-L295) 负责在会话中启动进程并设置 UIAccess。因此“核心运行在用户会话”不能直接证明核心是当前用户 SID、非 SYSTEM 或具有该用户音频同意。

P1 必须验证：

1. 采集/播放 worker 的实际令牌 SID、SessionId、交互桌面及当前登录/解锁状态；默认限定有明确同意的交互用户，不在服务 Session 0、登录屏幕或安全桌面自动采集。
2. 音频授权 IPC 单独实现并验证调用者身份、ACL、用户/会话绑定、会话随机数/代次及命令状态机。只改成 UserAccessOption 也不能凭空解决 SYSTEM 与桌面用户的正确授权关系。
3. 无可信用户 GUI、跨用户管道连接、调用者未知、锁屏/快速用户切换/RDP 会话迁移、IPC 断连或服务恢复时默认关闭。是否允许恢复必须有清晰且重新验证的策略，不能从机器级设置继承。
4. 许可只发送给目标 worker，不使用公开广播/历史事件重放恢复音频许可；防止恶意本地端点伪造状态、取消/接受提示及重放旧授权。

## 5. RustDesk 固定参考与音频链路复核

### 5.1 固定方法与不可变对象

raw.githubusercontent.com 多次读取失败，本次使用 GitHub Contents/Git Blob API 获取固定 ref，对 base64 内容在内存解码分析。未将 RustDesk/CPAL 源码、片段、译写实现、测试或产物写入本仓库。以下 URL 都绑定不可变提交；blob SHA 是 Git 对象 ID，不是发行包 SHA-256。

| 参考对象 | 固定 URL | 已核对 Git blob SHA |
| --- | --- | --- |
| Cargo manifest | [Cargo.toml](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/Cargo.toml) | `10025869435836f4a8de53a37b91cf851f550425` |
| 依赖锁 | [Cargo.lock](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/Cargo.lock) | `7b7263a5835a464d6384895a0900337a3884fe76` |
| 采集/服务主链路 | [audio_service.rs](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/src/server/audio_service.rs) | `4bb234f39072ffc78df5ff853e04691ac4805ca0` |
| 采集定帧 | [audio_capture.rs](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/src/server/audio_service/audio_capture.rs) | `27b2598355e168e7696a58858a027759469a3b0a` |
| PCM 交接队列 | [audio_capture_queue.rs](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/src/server/audio_service/audio_capture_queue.rs) | `c8033d08ef428daddbe82360dda36bf635777f06` |
| 编码 worker | [audio_capture_encoder.rs](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/src/server/audio_service/audio_capture_encoder.rs) | `e090c9f2c19e7ba04f63d434c4750311b27adbfb` |
| 采集错误/重启代次 | [audio_capture_error.rs](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/src/server/audio_service/audio_capture_error.rs) | `c5f944b89d341b86a591f8e94bd95f26c607fcf3` |
| 客户端解码/设备输出 | [client.rs](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/src/client.rs) | `597dc863af98e4a63a201e60f9669b9454f16302` |
| 网络消息路由 | [io_loop.rs](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/src/client/io_loop.rs) | `1b197a8384ea1f22476deee7e49fc32b974177e3` |
| 客户端播放回调/恢复 | [audio_playback.rs](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/src/client/audio_playback.rs) | `656533c5e41a3f4c89f74540350a2b19fe0e2d51` |
| RustDesk 许可证 | [LICENCE](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/LICENCE) | `0ad25db4bd1d86c452db3f9602ccdbe172438f52` |

可复核 API 示例：[manifest contents](https://api.github.com/repos/rustdesk/rustdesk/contents/Cargo.toml?ref=23e5bb471a040d62f0a6ad52591fb30784265b73)、[queue blob](https://api.github.com/repos/rustdesk/rustdesk/git/blobs/c8033d08ef428daddbe82360dda36bf635777f06)、[encoder blob](https://api.github.com/repos/rustdesk/rustdesk/git/blobs/e090c9f2c19e7ba04f63d434c4750311b27adbfb)。不要拿 API 错误页、404 或未完整显示的大文件当作成功阅读；本次对锁文件进行了独立完整解码并提取相关包。

### 5.2 Cargo CPAL fork：准确固定与限制

manifest 第 95 行不是 crates.io 默认 CPAL，而是 `rustdesk-org/cpal` 的 `osx-screencapturekit` 分支。固定 RustDesk lock 实际选择：

- CPAL **0.15.3**，git revision **`96d4da121b7d949677ac5b6887413a9185fd7f39`**。
- [fork Cargo.toml](https://github.com/rustdesk-org/cpal/blob/96d4da121b7d949677ac5b6887413a9185fd7f39/Cargo.toml) 声明 Apache-2.0；[fork LICENSE](https://github.com/rustdesk-org/cpal/blob/96d4da121b7d949677ac5b6887413a9185fd7f39/LICENSE) blob 为 `261eeb9e9f8b2b4b0d119366dda99c6fd7d35c64`，确为 Apache 2.0 正文。
- [fork WASAPI device.rs](https://github.com/rustdesk-org/cpal/blob/96d4da121b7d949677ac5b6887413a9185fd7f39/src/host/wasapi/device.rs#L558-L568) blob `d660aa8209e495671314e4df901f267677763827`：使用 EVENTCALLBACK；对 eRender 设备添加 LOOPBACK，并使用共享模式。这是从实现确认的能力，不是从 CPAL 名字或旧注释推断。
- Magnum Opus wrapper 固定为 **0.4.0** / [`588c6e1f9ed50c3a01fa64f3bd3e7cdb0378a114`](https://github.com/rustdesk-org/magnum-opus/tree/588c6e1f9ed50c3a01fa64f3bd3e7cdb0378a114)，同样不是本次依赖；不把该 wrapper 当作 libopus 本体或本次许可证清单。

[Microsoft Loopback Recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording) 明确：loopback 选择 render endpoint、共享模式；Windows 10 1703 前事件驱动 loopback 不会收到同样的采集事件。RustDesk 固定版本的 audio_service 也明确记载这个限制。**不能宣称此 fork 让 Win7 系统 loopback 正常工作，也不能宣称上游所有 CPAL 版本一概“不支持 loopback”。** 如本次 C++ 后续只实现事件驱动方案，必须限制已支持的 OS；旧 OS 另行实现/验证轮询或辅助 render 事件方案，不能无声挂起后报正在分享。

### 5.3 经复核的链路及可以借鉴的工程原则

1. **设备与格式：** audio_service 的 Windows 系统音频路径取得默认输出设备/config；语音输入是其他选择分支。主文件开头历史注释说 CPAL 不支持 loopback，不适合用作当前固定 fork 的实现结论。采集格式经转换、定帧及重采样后形成适合 Opus 的单/双声道、受支持采样率。固定版本对 44.1 kHz 按区间选择 24 kHz 后重采样，并非任何输入都原样作为 48 kHz 编码。
2. **回调到队列：** audio_capture 负责把任意回调块拼成约 10 ms 帧；PCM handoff 预分配 10 个包的池（名义约 100 ms，不等于端到端延迟）。submit 用 try_lock；竞争丢当前包，池耗尽取最旧待处理包复用，超大包拒绝并记数。它是有界、尽量不等待的回调设计，**不是 lock-free，也不是绝对无阻塞保证**。
3. **独立编码：** audio_capture_queue 创建 LowDelay Opus 编码器与独立 worker；audio_capture_encoder 检测序列不连续、作短过渡，调用 send_f32 编码并通过服务发送 AudioFrame，格式通过独立 AudioFormat 消息告知接收端。长零值静音门控是传输优化，不是采集许可开关。
4. **生命周期：** 每条采集流自己的错误标志使迟到回调不能重启替换流；中断或处理失败请求重建。worker 退出涉及唤醒、排空与 join。固定源码还明确承认 Rust 1.75/Win7 的 unpark 可能等待目标线程进入内核等待；不能把移出编码回调的改进虚报为彻底排除回调/停机卡顿。
5. **客户端：** io_loop 把 AudioFormat 与 AudioFrame 分发到音频线程，并有 disable_audio 判定；client 的 AudioHandler 根据格式创建解码器，decode_float 后进行播放格式/声道/采样率适配，经有界 PCM 环形缓冲送给 CPAL 输出回调。audio_playback 对缓冲竞争/异常、未就绪/缺样及 discontinuity 有处理。client 的 AUDIO_BUFFER_MS 为 3000，是容量参数，不是可照搬的低延迟目标，更不是我们的达标数据。

可迁移的是“有界队列、回调职责小、独立编码/发送、代次隔离、错误恢复、溢出/欠载统计”等一般原则。**不复制/翻译 RustDesk 的代码、函数体、测试、独特结构或补丁；不引入其 crate/静态库/二进制。** 本文不宣称对 RustDesk 做过完整安全审计，也不以其权限模型替代 Deskflow 本地用户边界。

## 6. 方案比较与构建/许可证边界

| 维度 | 直接 C++ / WASAPI + libopus（本次选择） | Rust / FFI + CPAL（本次不采用） |
| --- | --- | --- |
| 与现仓集成 | 现仓 CMake、C/C++、C++20，可直接用 Windows SDK COM/WASAPI 与 Opus C API | 新增 Cargo、Rust target/toolchain、桥接与双语言构建/打包 |
| 音频平台能力 | Windows loopback 选择/共享模式/事件恢复均显式控制；非 Windows 后端另审查 | 抽象多平台有价值，但仍需确定 fork、loopback 后端、平台特性与设备权限 |
| 安全和可靠性 | 必须自证 RAII/COM apartment、缓冲生命周期、线程退出、格式转换与边界验证；C++ 不是天然安全 | Rust 能降低部分内存风险，但跨 FFI 的所有权、回调线程、panic、ABI、DLL/CRT 生命周期仍需额外契约 |
| 许可 | 独立 libopus BSD 风格许可适合现有 GPLv2-only 组合，但须保留声明并审计真实版本/传递依赖 | 固定 CPAL 为 Apache-2.0，与 GPL-2.0-only 组合存在兼容障碍；FFI/动态链接不是绕过许可证的理由 |
| 本次结论 | 独立实现，不从 AGPL 翻译代码；不代表已有可交付实现 | 不增加依赖；若未来重新评估必须另行法律/架构批准，不能悄悄导入 RustDesk wrapper/fork |

### 6.1 本仓实际构建证据

[根 CMake:37–40、93–99](../../CMakeLists.txt#L37-L99) 使用 C/C++ 与 C++20；[173–194](../../CMakeLists.txt#L173-L194) 查找 Qt6 Core/Widgets/Network 及部署工具。[vcpkg 模板](../../cmake/vcpkg.json.in) 固定 baseline `d5ec528843d29e3a52d745a64b469f810b2cedbf`，已列 OpenSSL，可选 Qt；本审查基线未声明 CPAL/Opus。不能因为本地机器有库就宣称依赖/许可证已固定。

音频依赖应在后续 P1 关口记录实际选定的 libopus 版本、不可变 commit/下载 SHA-256、构建选项、静态/动态方式、架构、运行库与完整依赖清单；本文不改构建文件，也不虚构“已经接入 Opus”。关闭音频的构建不能强制查找/链接 Opus，不允许缺库时悄悄降级到不安全/未协商 PCM。ON/OFF 两种构建与安装包验证仍未执行。

### 6.2 分发义务（工程审查结论，不替代法律意见）

1. **Deskflow：** [根 LICENSE](../../LICENSE) 是 GPLv2；实际核心源码头为 GPL-2.0-only WITH LicenseRef-OpenSSL-Exception，不是可任意升级到 GPLv3 的 GPLv2-or-later。[REUSE.toml](../../REUSE.toml) 同时标注文档/构建 MIT、部分图标 LGPL-2.1-only 等，不能将整个仓库简化为 MIT。分发修改后的二进制必须履行 GPLv2 对完整对应源码、修改说明/版权、许可证及可用源码提供方式的要求，包含控制编译与安装的脚本；源码应与实际发行版本匹配。不能只放一个上游链接就宣称履行全部义务。
2. **OpenSSL：** [现有例外](../../LICENSES/LicenseRef-OpenSSL-Exception.txt) 只允许与 OpenSSL 的相关链接组合，不豁免其他代码的 GPL 义务，也不授权 CPAL/AGPL。仍须按实际 OpenSSL 版本保留其许可证/NOTICE 等要求。不能把“有 OpenSSL 例外”解释成对所有 Apache-2.0 依赖都有例外。
3. **libopus：** 本次独立参考候选为 Xiph Opus **v1.5.2 / commit [`ddbe48383984d56acd9e1ab6a090c54ca6b735a6`](https://github.com/xiph/opus/commit/ddbe48383984d56acd9e1ab6a090c54ca6b735a6)**，[COPYING](https://github.com/xiph/opus/blob/ddbe48383984d56acd9e1ab6a090c54ca6b735a6/COPYING) blob `75711467a3eb220f35f72afdb55a4734ee5cdb47`。这不是宣称已经采用该版本作为最终构建锁。许可允许修改与二进制分发；源码保留版权、条件、免责声明；二进制在随附文档/材料复现这些条款；不得使用列明组织或贡献者名字背书。COPYING 还链接免版税专利许可说明；免版税不等于无条款，实际版本/组合仍需复核。不需要因此引入 RustDesk 的 Magnum wrapper。
4. **CPAL：** 固定 fork 是 Apache-2.0，若另一个经批准且兼容的产品使用它，须附许可证、保留相关声明、标明修改，并在适用时保留上游 NOTICE 及专利条款；这些条件不能把 GPLv2-only 组合自动变兼容。[GNU 许可证兼容性说明](https://www.gnu.org/licenses/license-list.html#apache2) 区分 GPLv3 兼容与 GPLv2 不兼容。**本次不分发/链接 CPAL，亦不复制其实现。**
5. **RustDesk：** 固定 [LICENCE](https://github.com/rustdesk/rustdesk/blob/23e5bb471a040d62f0a6ad52591fb30784265b73/LICENCE) 为 GNU AGPLv3。其源码/衍生实现一旦进入发行物，会涉及 AGPL 对源码、声明及修改后网络交互提供对应源码等义务，且不能据此与本仓 GPLv2-only 随意合并。**仅做架构事实分析和参考链接，不引入 AGPL 代码/构建产物；换语言、FFI、动态库或子进程都不是自动洗掉版权义务的方法。**
6. **Qt、图标与 Windows 运行库：** 按实际选用 Qt 模块/商业或开源许可逐项处理，不能断言所有 Qt 都是同一种 LGPL；LGPL 组件分发需保留声明并满足相应源码、替换/重新链接等条件。图标保留现有 LGPL/版权归属。Windows SDK 系统 API 的使用不等于可随意捆绑系统 DLL；MSVC runtime 随发行需遵守可再发行许可。
7. **安装包核对：** [根 CMake:245–254](../../CMakeLists.txt#L245-L254) 安装根 LICENSE 和 OpenSSL exception，并不证明新的第三方声明会自动带入。[Windows 打包](../../deploy/windows/deploy.cmake) 及 [便携打包步骤](../../deploy/windows/pre-cpack.cmake.in) 仍需针对真实产物检查 Opus/Qt/OpenSSL 的许可材料、源码获取方式及依赖清单。本文没有审计完整 SBOM 或认证已发行安装包合规。

## 7. 必须阻止自动音频启用的条件

以下任一成立，**不得启动采集、不得发送/接受音频帧，不能为了“尽量工作”回退到麦克风、明文或旧协议。** 键鼠是否继续按既有策略运行，与音频 fail-closed 独立。

| 阻断条件 / 不安全假设 | 解除阻断所需证据（P1 或后续验收，未完成） |
| --- | --- |
| PlainText、仅 Encrypted、未知/变更/未核对指纹；只凭连接成功或电脑名授权 | 已验证的双向身份、证书与授权目标绑定、方向明确的显式用户许可；身份改变清空旧许可 |
| 权限仅来自机器级设置、服务命令、WorldAccess IPC、版本 hello 或广播事件 | 当前用户 SID/SessionId 与 worker、受保护 IPC、当前许可及会话代次完整绑定；跨用户负向测试 |
| SYSTEM/Session 0、登录屏幕、安全桌面、锁屏、注销或快速用户切换、RDP 会话身份不明 | 经审查的交互用户策略；未知状态立即关闭，恢复需重新验证而非无条件自动续传 |
| 对端旧版/能力未知、协商超时或能力拒绝、未证明扩展入口兼容 | 双方安全能力确认；未知消息不会破坏旧端的实测证据；无音频情况下键鼠行为不退化 |
| 独立音频端口/通道未绑定认证会话；仅以 IP/名称/端口认人 | 不可重放的通道绑定、超时、代次隔离、认证及加密；串会话/重放负向测试 |
| 设备消失/切换、格式不支持、OS 不支持事件驱动 loopback；默认回落麦克风 | 设备/格式/OS 状态验证；重新选择与许可策略明确；不把静音挂起报告为正在共享 |
| 未限定采集内容，误认为 loopback 只包含目标应用或当前用户声音 | 明确告知系统混音可能含通知、通话、其他应用，按实际 OS/会话行为审查；不能把普通 loopback 当进程隔离 |
| 队列/编码/网络回调可无限等待，消息/解码输出无上限，断连后残留 PCM | 有界内存与时延、非实时线程执行重任务、停机与清空证明、恶意帧/拥塞/欠载恢复测试 |
| 捕获到本地播放的远端音频再次回传，或多路订阅未经授权导致回路/泄漏 | 明确方向与接收者、回路规避及目标切换策略；不得仅从键鼠焦点推导发送权限 |
| RustDesk 代码/译写代码、CPAL 或未经审核依赖进入构建，Opus 来源/分发材料未锁定 | 独立实现及来源审查；实际构建依赖/许可证/发行包检查；必要时法律批准 |

### P0/P1 最终交接

- **已完成：** 本仓上述静态源码审查、RustDesk 指定版本与 CPAL 精确 revision 固定、音频服务/队列/编码/client 链路事实复核、直接 C++ 路线与无 CPAL/无 AGPL 导入边界、自动启用阻断清单及分发义务记录。
- **P1 仍待证明：** 用户/会话授权契约、身份绑定、旧协议无损能力协商、构建/许可证锁定与音频开关两态可构建；不能仅因本文存在就勾选通过。
- **没有声称完成：** P2–P5 的采集编码、网络传输、播放、GUI、兼容性/性能/设备/安全测试和打包验收。没有测量端到端延迟、音质或 CPU，也没有复现并修复现有 IPC 风险。
