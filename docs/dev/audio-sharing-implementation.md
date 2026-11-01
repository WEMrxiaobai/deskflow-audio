# 音频共享实现与验证记录（含P0 / P1历史证据）

更新：2026-11-02。当前1.27.6已集成三机双来源路由、统一配对、GUI和共享WASAPI混音，详见[当前集成说明](audio-sharing-integrated.md)。本文后半部分保留P0/P1阶段的工具、限制和测量证据，带有“当前/尚未”的历史表述只指当时原型。实机跨机延迟、长期稳定性及完整发布验收仍未完成。

## 主功能先行的体验入口

早期[双机开发预览](audio-sharing-preview.md)及临时邀请入口仅作历史开发工具。当前使用完整Deskflow集成版，在插耳机的电脑打开收听开关，复用原键鼠身份与信任；另外两台可以同时发送系统声音，支持逐来源音量/静音。真实跨机体验和物理延迟仍待实测。

## 实施状态

| 阶段 | 本轮结果 |
| --- | --- |
| P0 | 完成本仓身份、旧协议、IPC、Windows 会话与许可证静态审查；固定 RustDesk / CPAL 参考；选择原生 C++ / WASAPI + libopus，无 Rust/CPAL/AGPL 代码导入。详见 [P0 审查](audio-sharing-p0.md)。架构仍需项目评审。 |
| P1 | 已有可编译音频库、隔离开发探针、真实 Opus/SRTP UDP loopback、WASAPI 后端及自动化测试；已有本机一分钟5/10ms连续性短测（不是实机soak）；**两机物理端到端延迟、真实音乐重采样质量及设备兼容验收未完成**。 |
| P2 | 已复用原证书和角色信任库，以独立mTLS控制通道协商能力、路由及短期媒体凭据；音频由GUI进程持有，不经原world-access IPC下发密钥。已实现会话重试、键鼠断开/撤销gate；复杂网络及电源恢复仍待实机验收。 |
| P3 | 已接入三机、一个收听中心、两路直接UDP来源；每源独立解码/时钟、增益/静音和身份持久化，共用一个WASAPI混音输出。覆盖同机三身份回归和真实声卡输出，尚无三台物理电脑测试结果。 |
| P4 | 已有中文GUI收听开关、发送停止/本机播放选项、逐来源音量/静音、完整MSI及源码包。Windows主音量同步、逐来源停止传输菜单等原规划项仍未实现。 |
| P5 | 跨机听感/延迟、小时级稳定性、休眠/网络/设备矩阵和实际安装升级/卸载验收仍待完成；未宣称生产发布验收通过。 |

## 代码入口和线程所有权

- [音频独立构建](../../src/lib/audio/CMakeLists.txt)：可脱离 Qt 单独编译；主项目 `ENABLE_AUDIO_SHARING` 默认 OFF。关闭时不查找 Opus/SRTP，不添加音频协议消息。
- [引擎接口](../../src/lib/audio/engine/AudioEngine.h) / [实现](../../src/lib/audio/engine/AudioEngine.cpp)：一个 capture producer、一个内部 encoder worker；每来源一个外部 decoder worker、一个 device reader。公开说明的单线程所有权必须遵守，不能随意多写。
- [采集管线](../../src/lib/audio/dsp/CapturePipeline.h)：交错 f32 → 名义采样率/单双声道转换 → 跨回调 5/10ms 分帧 → 预分配 4-slot SPSC。满时丢当前新块；consumer 剔除陈旧旧块，producer 不移动 consumer 游标。与 RustDesk 池式“丢最旧”实现不同，但同样有界；序号即使丢块也推进。
- [Opus](../../src/lib/audio/codec/OpusCodec.cpp)：48kHz stereo、240/480 samples per channel、restricted-lowdelay、160kbps VBR、complexity 5、强制 stereo，无 DTX、无 FEC。查询真实 lookahead，不用帧长冒充算法延迟。
- [SRTP](../../src/lib/audio/transport/SrtpTransport.h)：libSRTP AES_CM_128_HMAC_SHA1_80，RTP v2/PT111，UDP ≤1200 字节，鉴权后才提供 metadata，明确 SSRC/key/generation 绑定，防重放、异常拒绝，无明文 fallback。新流需新密钥和 SSRC；密钥不接受 CLI、不落日志。
- [网络抖动缓冲](../../src/lib/audio/dsp/JitterBuffer.h)：8 packet slots，首次启动 deadline=5ms，不等到容量满。乱序、晚包、丢包 deadline 与解码后的 PCM 缓冲完全分离；每 tick 最多处理 8 个决策。
- [网络媒体时钟](../../src/lib/audio/dsp/NetworkClock.h)：仅认证包更新的有界 PI/PLL，未来 deadline 步长 ±2000ppm，积分 ±1000ppm，不跳当前 deadline。修复固定 deadline 在设备频偏下数十秒后永久晚包的问题。arrival 含网络与调度变化，估计值不是物理声卡频偏测量。
- [PCM 播放](../../src/lib/audio/dsp/Playback.h)：30ms 有界容量、device reader 发现 >25ms 积压时丢旧至25ms；欠载约 5ms 淡出到零、恢复/断点平滑、每源独立增益/静音、远端混音峰值限制 .98。不限制 Windows 最终混入的本地应用声音。
- [PCM 时钟漂移](../../src/lib/audio/dsp/ClockDrift.h)与[相位快照](../../src/lib/audio/dsp/PlaybackClock.h)：每来源独立水位 PI，worker采样ring加最近callback尚未按名义Fs播放的PCM，以12ms归一化总量为prewrite目标，消除仅采ring造成的bulk callback相位偏置。限制callback≤10ms；单次原子一致性观察不自旋，欠载/trim/flush/过旧快照跳过，PI按实际elapsed每tick至多一次且排除catch-up。独立模型及真实引擎联合虚拟时钟稳态已验证；**仍非真实WASAPI padding/IAudioClock，实机长期漂移未验收**。目标提高带来的延迟预算不能隐瞒。详见[诊断与验证手册](audio-sharing-p1-validation.md#6-播放水位反馈的历史失败与相位归一化验证)。
- [WASAPI](../../src/lib/audio/windows/README.md)：默认 eRender/eConsole 系统回环，不是麦克风；shared render，与本机声音共存；默认端点/格式通知、独立 worker/MMCSS、fresh-client fallback、协作 stop/join、端点主音量 helper。
- [开发探针](../../src/apps/deskflow-audio/main.cpp)：仅 127.0.0.1 UDP，进程内随机临时密钥，不接受外部 IP、端口或密钥，也不打开现有 WorldAccess IPC。它不是第二套配对系统。

采集/播放回调不编码、不做网络/日志、不加锁/动态分配。Windows encoder worker 使用线程私有 high-resolution waitable timer；普通 `sleep_for(250us)` 在本机实测可变成约 15ms 调度，耗尽预算。没有调用全局 `timeBeginPeriod`。

## 格式、静音、恢复规则

1. 引擎仅协商 48k stereo、5/10ms。设备端显式支持 8–192kHz mono/stereo；多声道设备不偷偷丢声道，而是拒绝。WASAPI 样本容器支持 float32/i16/i32，包括 24-valid-in-32；packed24 不支持。
2. 已实现并接入 [BandLimitedAdapter](../../src/lib/audio/dsp/BandLimitedAdapter.h)：1024 相位区间的 Blackman windowed-sinc FIR，系数构造时计算，动态 ppm 只改整数相位步长，不重建/分配。不同采样率的 capture 和全部 receiver 使用它；48k 同采样率 capture 仅单双声道适配，无需滤波。64 × max(1,input/output) 个偶数 taps，固定 lookahead=taps/2 输入帧；96→48 为 64 帧（约0.667ms）、192→48 为128帧（约0.667ms）、48→48 receiver 为32帧（约0.667ms）。首次 callback 不足完整媒体帧时等待下一次贡献，不补假音、不隐藏 lookahead。保留 [StreamAdapter](../../src/lib/audio/dsp/StreamAdapter.h) 作为线性对照及无需采样率转换的通道映射；不能将其当抗混叠实现。当前 FIR 的保守 cutoff 牺牲较低一侧 Nyquist 顶部约10%，是待音质评审的原型参数，不宣称全频段透明或生产音质验收通过。
3. exact-zero gate 默认关闭。显式启用必须提供可信 `SignalSink`；200ms 全零后停发，非零低电平/subnormal 不截断；媒体序号照样消耗。恢复信令必须先通过可信控制层安排 receiver reanchor。不能凭 UDP 自动改格式/代次。
4. PLC 最多连续两包，之后归零，不重复最后音频。超过一秒没有认证媒体、严重 decoder 调度停顿或媒体 phase 跳变 >40ms，标记 `requiresReanchor`，清旧播放积压，不悄悄接受不连续流。
5. WASAPI 静音时可能完全没有采集回调，媒体计数不会自行代表墙钟。这种恢复必须由后续控制面 resume/reanchor 处理；当前探针明确失败退出，不虚报恢复成功。
6. trusted reanchor 不重置 SRTP 防重放/ROC、不改 key/SSRC/格式、不解除静音。writer 请求 reader 清旧 PCM，**收到 reader ack 前的新解码 PCM 明确丢弃并计数**，不是无损恢复。网络状态和重采样状态在 worker 上重置，不在输出回调重建设备。
7. 设备/编码格式变更需要停止并销毁旧引擎，以新格式、新 key/SSRC/generation 重建；WASAPI 后端会重开并报告代次。探针检测代次变化即停，尚未实现管理器自动协调重建。
8. 任何半序列空间以上的未保护媒体 gap，或 SRTP 包索引耗尽，发送端停止并要求新会话材料，禁止重用 nonce/index。

## 依赖及复现构建

| 依赖 | 固定归档 | SHA-256 |
| --- | --- | --- |
| Opus 1.5.2 | <https://codeload.github.com/xiph/opus/tar.gz/refs/tags/v1.5.2> | `9480e329e989f70d69886ded470c7f8cfe6c0667cc4196d4837ac9e668fb7404` |
| libSRTP 2.7.0 | <https://codeload.github.com/cisco/libsrtp/tar.gz/refs/tags/v2.7.0> | `54facb1727a557c2a76b91194dcb2d0a453aaf8e2d0cbbf1e3c2848c323e28ad` |

两者 BSD 风格许可，随附原始 [Opus COPYING](audio-licenses/opus-COPYING.txt) 与 [libSRTP LICENSE](audio-licenses/libsrtp-LICENSE.txt)。未来二进制分发必须包含这些声明并满足根 GPL 及所有实际依赖要求；当前没有正式 install/packaging 路径。**libSRTP debug logging / ERR_REPORTING_STDOUT / ERR_REPORTING_FILE 必须关闭，不能启用运行期 debug modules，上游调试可能输出派生密钥。**

推荐从 x64 Developer PowerShell（CMake ≥3.24、VS2022 C++ 与 Win10 SDK）运行：

```powershell
# 显式选择下载固定哈希依赖，不默认联网下载。
cmake -S src/lib/audio -B build/audio -G "Visual Studio 17 2022" -A x64 `
  -DDESKFLOW_AUDIO_FETCH_DEPENDENCIES=ON
cmake --build build/audio --config Release --parallel
ctest --test-dir build/audio -C Release --output-on-failure
build/audio/Release/deskflow-audio-probe.exe --self-test 3 5
build/audio/Release/deskflow-audio-probe.exe --self-test 3 10
```

已有依赖时，保持 fetch OFF 并传 `-DCMAKE_PREFIX_PATH=<Opus和libSRTP安装前缀>`。主项目可显式 `-DENABLE_AUDIO_SHARING=ON`，但仍需原有 Qt/OpenSSL 构建环境。依赖实际版本/构建选项由调用者负责，已安装库路径模式不自动强制来源哈希。关闭主开关的默认项目不新增依赖。

开发探针还有两个**显式设备操作**：

```powershell
# 会采集当前系统混音；不存盘、不回放，避免反馈。
build/audio/Release/deskflow-audio-probe.exe --capture-probe 3 5
# 会播放低电平440Hz测试音；不采集系统混音、不改系统音量。
build/audio/Release/deskflow-audio-probe.exe --render-probe 3 5
```

探针从不同时 capture+render。接收中心不允许重新采集远端播放混音的产品级路由保护尚待 P2/P3，不能拿 localhost probe 当三机共享。

## 本机验证结果

环境：Windows x64，MSVC 19.44.35225 / VS2022 toolset 14.44.35207，Windows SDK 10.0.26100，CMake 3.31.6；Opus/libSRTP 静态 Release `/MD`。

- 八组 CTest：DSP、Transport、Codec、Engine、NetworkClock、Resampler、RtSafety、WASAPI。Debug 8/8通过；Release七组通过，RtSafety因无Debug CRT明确Skipped；最终日志见 [Release](../../build/audio/ctest-release.txt) / [Debug](../../build/audio-fetch/ctest-debug.txt)。支持安装依赖前缀和固定源码 FetchContent 两种路径，均实际构建验证。
- DSP Debug 与 Release：513,318 项检查；跨回调/有界过载、44.1k 比例一致性、欠载归零/恢复、多源、静音、序号 wrap、SPSC 双线程压力和十分钟 +200ppm 模拟。
- SRTP 九组测试：逐位篡改、截断、错误 key/SSRC、重放、乱序、MTU、ROC/时间戳 wrap、旧代次、一次性 key、明文拒绝。
- 带限重采样 360,489 项检查：96/192→48 的30k混叠相对linear分别约−97.69/−97.75dB；±5000ppm的24k阻带最坏−78.28dB，20k通带最坏−0.025dB；44.1→48的1/10/18k增益偏差≤0.00181dB；8→48的7k image −73.50dBFS；phase-table+float系数对独立double FIR峰值误差8.28e−7；分片逐位一致、背压、rational frame count、动态ppm/重置零分配检查通过。不是全阻带扫频或实机听感/CPU尾部验收。
- 网络时钟 28 组：5/10ms，±100/500/1000ppm，每组十分钟虚拟时间，有/无 ±1ms 抖动，0 拒包/0 loss，最大预期驻留约 7.56ms。**模拟不是实机长时验收。**
- WASAPI 实际本机端点 smoke：render 与 loopback 各打开 3 次、各 29 callbacks；12 轮 stress handle 196→196；只输出零、不改变主音量。MSVC `/analyze` 空缺陷报告。
- 新增 [RtSafety](../../src/lib/audio/tests/RtSafetyTests.cpp) MSVC Debug CRT hook检查（先自测hook有效）：覆盖C/C++ allocation/reallocation/free，FIR process/ppm/reset、采集callback、两源mix/控制及flush/underrun均为零。范围仅受测应用调用线程，不包括OS/驱动/直接HeapAlloc，不是硬实时认证；Release明确跳过。
- 新增[离线性能工具](../../src/lib/audio/tests/AudioBench.cpp)，统计5/10ms编解码/SRTP、常见设备格式FIR与双源mix的mean/p95/p99/p99.9/max、合成tone带宽和真实Opus lookahead。详见[可复现验证手册](audio-sharing-p1-validation.md)；不是CPU占用或声卡回调调度/物理延迟证据。
- 音频C++头/源码已使用仓库clang-format20.1.0整理并检查；Windows头文件依赖顺序通过分组保留。
- 当前工具链缺少 ASan runtime，ASan 没有执行成功，不计为通过。
- 全 GUI/安装器 ON/OFF 构建未验证：本机只有已安装的 Qt 运行库，没有完整 Qt 开发包；独立音频构建不依赖它。

历史基线 **3 秒 localhost SRTP/Opus 软件探针**（不访问声卡；下表采集于原20ms容量/10ms trim策略，不能代表现候选30/25策略）：

| 项目 | 5ms | 10ms |
| --- | ---: | ---: |
| 采集/发送包 | 600 / 600 | 300 / 300 |
| 解码包 | 599（结束时1包在jitter） | 300 |
| capture expired / network late / jitter loss | 0 / 0 / 0 | 0 / 0 / 0 |
| PCM 队列驻留均值 / 最大 | 489.2 / 930.9µs | 492.8 / 950.9µs |
| 编码均值 / 最大 | 66.59 / 137.5µs | 95.43 / 171.0µs |
| jitter 实际驻留均值 / 最大 | 5.579 / 6.787ms | 5.633 / 6.513ms |
| 解码均值 / 最大 | 29.98 / 105.0µs | 55.04 / 82.7µs |
| PCM 播放实际驻留均值 / 最大 | 3.310 / 9.519ms | 3.118 / 4.337ms |
| receiver FIR lookahead | 32 媒体帧 | 32 媒体帧 |

历史中间6ms目标/30/25策略一分钟10ms本机回环：6000发送/6000解码、0expired/late/loss，511启动欠载帧，PCM播放驻留mean9.227ms/max14.967ms、末校正+21.8ppm。它只证明这次无物理频差的localhost连续性，不推翻手工诊断中的漂移失败；[10ms日志](../../build/audio/probe-final-60s-10ms.txt)。5ms一分钟12000发送/11999解码（末包留jitter）、同样0expired/late/loss与511启动欠载，PCM驻留mean9.136ms/max14.792ms；[5ms日志](../../build/audio/probe-final-60s-5ms.txt)。

新版12ms归一化目标各一分钟本机回环：5ms12000发送/11999解码、10ms6000/6000，均0expired/late/loss、512启动欠载帧；PCM驻留mean11.032/10.914ms、max14.955/14.724ms，末校正+20.68/+16.29ppm。日志：[5ms](../../build/audio/probe-phase-60s-5ms.txt) / [10ms](../../build/audio/probe-phase-60s-10ms.txt)。12ms目标增加稳态预算但取消采样偏置，并非免费降低延迟。真实引擎联合基线30case、设备扩展48case、有限网络交付16case各420s虚拟时间、末300s均无PCM损失与jitter loss，另有确定性device/decoder线程交错进度回归；详见[相位与联合模拟](audio-sharing-p1-validation.md#6-播放水位反馈的历史失败与相位归一化验证)。

这些是单次短测，受本机负载影响，不是百分位或延迟保证。启动阶段有 511 个输出欠载帧（含 FIR 首包 lookahead），单独计数，未隐藏；无无限 PLC。容量、实际占用快照、启动水位、驻留时间分别输出。PCM 播放环形缓冲为各已入队帧预分配写入时间戳，以一次输出回调入口时钟记录实际消费驻留均值/最大；不逐样本查询系统时钟。它测的是写入至声卡回调消费，不包括该 callback 后的声卡排队/硬件播放，也不是逐帧分位分布；不能以水位换算值或该软件驻留值冒充物理播放延迟。

## 必须补完的 P1 验收

1. 已提供[离线脉冲分析工具与接线说明](../../tools/audio/README.md)和23项合成回归：支持16/24/32-bit双声道PCM WAV，输出median/p95/p99/max和未匹配比例，不录音、不改音量。仍须两/三台 Windows 真实声卡物理脉冲：同时记录来源参考信号和耳机输出，说明参考点、录音硬件/采样率、对齐方法和重复次数；报告 median/p95/p99/max 与丢脉冲比例，不能用 ping、队列配置或同进程时间戳代替。
2. 对照 5/10ms、设备周期、Opus/仅本机 PCM 对照、带宽与 CPU；记录 Opus lookahead 和采集/输出硬件延迟，非只编解码耗时。
3. 分别记录采集 PCM 入队/出队、网络 arrival/deadline/解码、PCM 播放写入/消耗的占用与驻留分布；回调预算超时、drop、late、PLC、underrun、CPU 及音质评估。
4. 对已接入的抗混叠重采样补全阻带/通带扫频、多音、极端比例与动态ppm CPU尾部测量，并测试原生44.1/48/96/192k真实设备、样本容器和声道；不接受“能发声”等同音质验收。
5. 静音无callback/resume、设备拔插、无设备重试、默认端点/格式变化、休眠/换用户的管理器恢复与授权清理；先补可信控制接口，不能凭UDP接纳新流。
6. 旧端兼容、安全凭据轮换/撤销、权限跨SID/Session负向、路由环路、真实两来源独立性、Wi-Fi以及至少数小时实机漂移。

[P0 审查](audio-sharing-p0.md)与[独立审查修复记录](audio-review-notes.md)保留安全边界及未完成事项。上述关口通过之前不勾选原计划的完整产品验收项、不发布生产级共享开关。
