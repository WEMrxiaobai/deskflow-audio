# P1 音频原型复现与验收清单

本文件是可执行验证手册，不代表验收通过。[实施记录](audio-sharing-implementation.md)区分已完成的软件测试与尚无证据的硬件测试；[原计划](../../AUDIO_SHARING_PLAN.md)保持不变。

## 1. 安全边界及本轮可测范围

- 音频构建默认关闭。开发探针只在进程内使用临时密钥及 localhost UDP，没有远程监听入口、密钥 CLI 或现有 IPC 授权入口。
- `--self-test` 不访问音频设备。`--capture-probe` 会读取当前系统混音，但不记录/回放音频。`--render-probe` 只播放低电平合成音，不捕获系统声音。设备测试必须显式执行。
- **当前不支持跨机原型运行。** 未接入可信控制面、会话绑定凭据下发、统一用户授权前，不添加任意 IP/裸密钥入口来“完成跨机测试”。下面跨机项目是下一阶段授权完成后的实验规范，不能在当前版本勾选通过。
- 原始音频不写诊断日志；密钥、原始PCM、完整UDP载荷不能进入测试附件。物理测量录音由用户自行创建/保管，并根据其内容决定是否可分享。

## 2. 独立构建与确定性测试

需要 Windows x64、VS2022 C++、Windows 10 SDK、CMake≥3.24。无需 Qt 或虚拟驱动。首次构建显式联网获取固定哈希的 Opus/libSRTP：

```powershell
cmake -S src/lib/audio -B build/audio -A x64 -DDESKFLOW_AUDIO_FETCH_DEPENDENCIES=ON
cmake --build build/audio --config Release --parallel
ctest --test-dir build/audio -C Release --output-on-failure
cmake --build build/audio --config Debug --parallel
ctest --test-dir build/audio -C Debug --output-on-failure
```

八组 CTest：DSP、Transport、Codec、Engine、NetworkClock、Resampler、RtSafety、WASAPI。这些短测试不替代第6节独立长时相位/联合引擎虚拟时钟检查；历史失败诊断刻意复现旧策略反例，并不注册为验收CTest。**RtSafety 只在 MSVC Debug CRT 下运行**；Release/非MSVC退出77，由CTest明确标记Skipped，不把跳过算作零堆分配证据。Debug及其依赖必须使用匹配的Debug运行库；不要把只安装Release的库用于Debug链接。

[RtSafety 测试](../../src/lib/audio/tests/RtSafetyTests.cpp)先验证hook确实能看到C malloc/realloc/free、C++ new/delete和aligned allocation，然后只在调用线程的guard期间计数。覆盖FIR process/ppm/reset、44.1/48/96/192k capture回调、两源混音/静音/音量、reanchor读端清理和欠载路径，结果均应为零。encoder另线程的构造/编码分配不计入回调。范围不包括Windows/驱动自身的内部堆、第三方Release DLL、直接HeapAlloc，也不证明没有锁或没有调度停顿；不得表述为全进程实时认证。

## 3. 离线性能、带宽和算法延迟对照

[离线性能工具](../../src/lib/audio/tests/AudioBench.cpp)只使用内存与单调时钟，不播放、采集或联网；在正式测量前每项预热100次。以下输出机器可读JSON：

```powershell
build/audio/Release/deskflow-audio-bench.exe 10000 > benchmark-release.json
```

参数范围200–100000，默认2000。记录每项 mean/p95/p99/p99.9/max、名义媒体周期、p99/媒体周期比。case包括5/10ms Opus编解码、SRTP protect/unprotect、媒体48k与32/44.1/48/96/192k mono/stereo FIR（含动态ppm更新）、两路5ms混音及驻留遥测。带宽为合成997Hz低电平tone的SRTP字节数，含RTP及认证tag，**不含UDP/IP/链路层开销，不代表音乐VBR带宽**。Opus lookahead使用真实查询值。

记录CPU型号/核心数、OS/驱动、编译器、依赖版本/构建配置、电源方案、后台负载、迭代次数、源代码revision及时间。不修改电源方案，不自动关闭应用。结果只衡量串行热缓存处理：

- 不是实际CPU占用率；不能把MAC或处理耗时简单当成任务管理器CPU%。
- 不是设备callback超时、OS调度尾部、端到端延迟或长期多流负载证明。
- 单次2000样本的p99.9仅少数样本支撑，需多轮/更多样本及低端目标机重测。
- 按实测最短callback周期而非固定5ms预算讨论门槛。若链路单项处理超过预算，应优化或收窄支持范围，不靠加深队列掩盖。

### 本机单次性能证据（2026-11-01）

AMD Ryzen7 8745HS（8核16线程）、平衡电源方案、MSVC19.44 Release、每case预热100/测量10000次，非受控空闲实验室负载；[完整JSON](../../build/audio/benchmark-release-final.json)。Opus真实lookahead均120媒体帧（2.5ms）；5/10ms合成块SRTP带宽196.8/178.4kbps（不含UDP/IP）。这不表示物理端到端只有2.5ms，也不表示5ms/10ms音质等价。

| 处理 | mean / p99 / max（µs） |
| --- | ---: |
| Opus encode 5ms | 18.54 / 32.20 / 183.8 |
| Opus decode 5ms | 7.20 / 13.10 / 61.5 |
| Opus encode 10ms | 28.20 / 48.30 / 126.0 |
| Opus decode 10ms | 11.80 / 20.50 / 127.6 |
| FIR 192k stereo→48k stereo，约5ms块 | 162.24 / 253.13 / 335.5 |
| 双源5ms mix（含驻留遥测） | 7.84 / 13.40 / 65.6 |

192k mono→48k stereo另见单次max1161.4µs，不能隐藏为平均162µs；最短真实callback周期若短于5ms仍须独立预算。串行热缓存bench不证明callback deadline、CPU占用率、多流压力或可发布性。

## 4. 软件链路对照与本机WASAPI检查

```powershell
build/audio/Release/deskflow-audio-probe.exe --self-test 60 5
build/audio/Release/deskflow-audio-probe.exe --self-test 60 10
# 以下显式访问设备：
build/audio/Release/audio-wasapi-tests.exe --smoke
build/audio/Release/deskflow-audio-probe.exe --capture-probe 10 5
build/audio/Release/deskflow-audio-probe.exe --render-probe 10 5
```

分别记录四slot采集队列、八slot网络jitter、30ms PCM ring的容量、实际水位和驻留均值/最大；加上drop/expired/late/loss/PLC/zeros/underrun和codec处理耗时。FIR lookahead另列，不从软件驻留指标推算完整物理延迟。启动欠载及结束时还在jitter内的包独立报告，不从统计中无说明删除。

WASAPI smoke只播放零、读取而不修改主音量。capture-probe需要系统有持续音频callback；静音无callback时可能明确失败并要求trusted reanchor。这是已知管理器缺口，不是自动恢复成功。端点改变导致代次变化时探针停止，不在旧格式上续播。

## 5. 尚待完成的硬件/跨机矩阵

每项记录“未测/通过/失败”、环境、原始指标、已知限制；不能以自测通过替代。

| 项目 | 场景与指标 |
| --- | --- |
| 两/三台 Windows 物理延迟 | 获得可信会话/授权支持后，用同步双通道记录来源物理参考与耳机输出；5/10ms，wired/Wi-Fi分开，median/p95/p99/max及未匹配比例 |
| 设备与格式 | 原生32/44.1/48/96/192k、mono/stereo、float/i16/24-valid-in32/i32；不支持格式明确失败，不静默丢声道 |
| 本地声音共存 | 远端音频与本机应用同时播放；不改远端主音量、不抢占独占模式；系统总音量读写交互验证 |
| 过载/网络 | CPU竞争、丢包/乱序/重复/晚包、短断网、UDP不通；队列有界，旧PCM丢弃后恢复，无持续增延迟 |
| 静音与平滑 | 精确零停发/恢复、无capture callback、极低非零/subnormal、跨callback约5ms淡变；听感与波形双验证 |
| 设备生命周期 | 默认端点切换、拔插所有端点、设备占用、格式变化、无设备重试；新key/SSRC/generation且无旧积压 |
| 会话和恢复 | 休眠/锁屏/注销/快速用户切换/RDP，可信管理器重建；未知用户/会话关闭，不借旧IPC继承许可 |
| 多源与漂移 | 两远端各至少2小时，独立mute/gain、一路故障不拖累另一路、不重新采集接收中心远端混音 |
| 权限和旧端 | 双向身份/目标绑定、撤销立即停止、旧端不接新消息；跨SID/Session、伪造IPC/UDP和过期凭据负向测试 |

## 6. 播放水位反馈的历史失败与相位归一化验证

[手工时钟诊断](../../src/lib/audio/tests/PlaybackClockTests.cpp)保留历史失败，**不再是当前引擎策略**，也不是验收测试：其退出0表示复现了旧配置病态以及6ms候选仍失败，不能表示校时成功。它用独立producer/delivery/reader事件、连续fractional occupancy隔离包边界采样问题，不含真实FIR整数相位、网络PLL、OS/设备调度，所以是排除性模型证据而非实际硬件结论。

```powershell
build/audio/Release/deskflow-audio-playback-clock-diagnostic.exe > playback-clock-diagnostic.csv
```

诊断含737个各600秒的case，统计末300秒：288个旧10/15ms trim对照、448个6ms target/25ms trim/30ms capacity/Kp3候选，以及一个超范围20ms callback反例。候选覆盖P5/10、C=128/48ms、5ms、10ms、4.113ms，4个phase，0/±100/±500/±1000ppm，精确交付/10ms批交付。非极限漂移的320个候选case中，76个稳态欠载、24个trim；无full-ring reject或饱和不能替代零损失验收。

明确反例：P10/C10、phase5ms、source−100ppm、无批交付，末300秒欠载约566.03frames、平均校正−60.67ppm、末值+28.58ppm，trim/reject均零。**因此不能仅调整阈值宣称已修复漂移。** producer边界采样reader的bulk sawtooth会形成慢phase beat，需要后续按实际device padding/clock、或固定本地时钟网格的time-weighted/phase-normalized反馈重设计；tick catch-up也不能把同一时刻多个包当多个独立时间步反复积分。

当前引擎已改为[相位快照](../../src/lib/audio/dsp/PlaybackClock.h)：callback前标记写入中，完成后发布callback entry timestamp和实际PCM帧；worker只尝试一次原子一致性快照（不自旋、不锁、不分配），在ring水位上加`max(0, callbackPCM - Fs*elapsed)`，总量目标12ms。10ms最大callback需要一次取足10ms，即使恰在下个producer前，所以目标不能只设6ms；2ms reserve后，正常postwrite峰值≤12+10=22ms，低于25ms软trim/30ms容量。快照时间是callback进入时间而非render处理结束时间。任何underrun、flush、trim令该callback快照失效；未初始化、逆序、超过20ms旧证据、并发重叠均跳过。短暂重叠保留有界elapsed基线，避免交替失败/成功只重建基线而长期无PI进度；实际失效快照清基线。正常worker在decode之后、观察前读取新的本地时钟，避免tick入口时钟早于解码期间已完成callback而持续拒样；虚拟时钟测试显式提供feedbackNs。[引擎回归](../../src/lib/audio/tests/EngineTests.cpp)已用decoder clock注入点与独立device线程的semaphore确定性交错：每次decode后先让callback完整结束，timestamp晚于tick入口，再观察新时钟，要求首120包>80次有效更新；之后交替过期时间样本/正常样本，要求≥50次冲突且继续>45次更新，不允许反复重建基线而停滞。注入函数仅在decoder worker调用，生产空指针默认读本地时钟；测试中的等待不在音频callback或生产代码发生。该case证明有限交错的进度，不是所有调度条件的wait-free认证。空render/mix不改时钟或flush。PI用实际elapsed，只更新每tick首个及时解码包；catch-up批量既不重复积分，也不把缺少待写包的水位误当时钟频差。PLC/零填充、断点恢复及PCM拒写被检测后另冻结30ms（一环容量），重建采样时基；拒写可能发生在本tick的prewrite观察之后，冻结不回滚已经发生的那次更新，不承诺包含损失的当次样本绝不影响PI；引擎回归显式检验单包丢失前反馈有更新、恢复冷却窗口校正和更新计数不变、随后更新恢复；空render/mix也验证不得消耗旧PCM或提前确认trusted flush。`ReceiverStats`及软件probe输出updates/conflicts/invalid/cooldown，避免只以校正值不变推断成功冻结。

仍显式拒绝超过ceil(10ms)的输出callback并报`UnsupportedPlaybackQuantum`。**这是名义callback时钟近似，不是实际WASAPI padding/IAudioClock测量**：pending假设提交PCM从callback entry起按名义Fs播放；真实设备padding、callback调度抖动及动态quantum还未验收。容量/目标不是完整物理延迟，也不能忽略目标从6提高至12ms的延迟预算变化。

```powershell
# 独立计数模型，包含负对照（1520个正向case+2负对照）
build/audio/Release/deskflow-audio-playback-phase.exe > playback-phase.csv
# 真实Opus/SRTP/jitter/PLL/FIR/PCM ring虚拟时钟集成，无设备访问
build/audio/Release/deskflow-audio-engine-clock.exe > engine-clock.txt
build/audio/Release/deskflow-audio-engine-clock.exe --extended > engine-clock-extended.txt
# 可选有限网络交付验证，任何失败均保持非零退出：
build/audio/Release/deskflow-audio-engine-clock.exe --network > engine-clock-network.txt
```

[相位模型](../../src/lib/audio/tests/PlaybackPhaseTests.cpp)真实运行：每case1200s统计末300s，P5/10、C128/240/441/479/480、4phase、两个同时事件顺序、warm/empty、integer output+fractional phase。800个0/±100/±500ppm正向以及400个空启动/欠载快照冻结恢复case稳态零trim/reject/underrun，最大mean校正误差0.0011ppm。另320个±1000ppm边界case稳态无损，但饱和没有余量恢复12ms目标，平均水位可偏448.59frames，**不能宣称边界目标调节通过**。启动阶段仍有loss/trim/reject，最大1425/644/480帧，不能把稳态结果扩张为全程无损。负对照6ms的ring-only与归一化目标均复现欠载；避免只有有利模型。原始[模型CSV](../../build/audio/playback-phase.csv)。

[真实引擎联合模拟](../../src/lib/audio/tests/EngineClockTests.cpp)已在Release运行30个case：P5/10、C128/479/480@48k、source 0/±100/±500ppm，每case420s、统计末300s，使用独立500µs worker poll、真实Opus decoder/SRTP/NetworkClock/jitter/FIR/PCM ring和显式虚拟callback时钟。全部末300s零trim/reject/underrun，零jitter lost/late，最大mean校正误差0.18ppm，未触发reanchor；[原始日志](../../build/audio/engine-clock-final.txt)。末瞬时校正有约±30ppm量化/worker轮询波动，不能报告为物理时钟测量。编码fixture仅首帧真实编码、后续重复有效码流并用不同SRTP packet index加密，不测音质。扩展`--extended`另48case已通过：44.1/48/96/192k × P5/10 × ceil(2.667/9.979/10ms) callback ×漂移正负，±100/500ppm幅值、phase0/.001/.999及worker0.5/1/2ms轮换；不是全笛卡尔或完整pairwise覆盖。每case420s末300s同样零trim/reject/underrun、全程零jitter lost/late、mean校正误差<1ppm；[扩展日志](../../build/audio/engine-clock-extended.txt)。仍未验收延迟callback、真实padding、长期网络批交付和跨线程压力，不能扩大为所有时钟场景通过。

`--network`另16case已通过：P5/10、44.1/48k、ceil(10ms) callback、±500ppm、0..1ms确定性正向交付抖动，加0/1ms接收轮询量化；每case420s末300s同门限，全程零jitter lost/late、稳态零PCM损失且mean校正误差<1ppm；[网络交付日志](../../build/audio/engine-clock-network.txt)。源媒体时钟保持独立；并非把source period本身当网络delay。该量化小于packet period，**不会实际形成同次交付多包**，不能算批量追帧或乱序验证；后者仍待补。fixture用固定整数函数生成delay，不代表真实网络概率分布。

10s本机probe新增反馈计数实测：1999包解码、1992次更新、0冲突/失效、6次启动cooldown，0trim/reject、512启动欠载；[日志](../../build/audio/probe-feedback-10s.txt)。末校正−731ppm属于空启动目标建立过程，不能用短测末值当设备频差；依然报告而不隐藏。它仅证明该次存在反馈进度，不替代收敛和并发压力验收。

主开关仍OFF，实机漂移/音质/物理延迟门禁保持未过。

物理WAV分析步骤见[双声道分析工具说明](../../tools/audio/README.md)。匹配数与未匹配率必须和延迟分位数同时报告，不能只挑成功匹配对隐藏断音。实测结果未齐之前维持feature OFF，不发布“一键三机收听”或20–40ms保证。
