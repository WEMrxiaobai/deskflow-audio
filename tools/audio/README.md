<!-- SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception -->
# 双声道物理延迟离线分析

`analyze_latency.py` 只读取**用户明确录制并提供**的 WAV；不访问音频设备、不录音、不播放、不调整系统音量、不联网，也不写出任何音频内容。仅依赖 Python 标准库和 numpy。不要把软件日志时间戳或估算值冒充物理测量。

## 接线与可复现条件

1. 使用同一台同步双通道采集设备，channel0（第一个通道）接被测链路起点的**真实物理参考脉冲**，channel1（第二个通道）接耳机的物理输出采集装置，例如固定位置的测量麦克风/耦合器。电平和接线必须适合设备额定范围；不要将功率输出直接接入不兼容输入。
2. 由用户自行设置并录制稀疏脉冲或短 burst。建议实际激励间隔 **800–1000ms**；默认同通道最小事件分离 500ms、最大匹配延迟 200ms。激励间隔应大于事件分离时间与最大延迟抖动之和，避免接收端相邻起点过近被合并。每个 burst 的持续时间应远小于 500ms，并在其间留出低于阈值的静音。
3. 固定耳机、麦克风位置、输入增益、系统音量、采样率、设备/系统音效、软件版本和链路配置，记录这些实验条件。禁用自动增益/噪声门等会改变脉冲起点的处理。使用相同信号同时接入两采集通道，单独评估通道固定偏差；本工具不自动校准、不扣除偏差。
4. 开始和结束录音时保留足够余量，尾部至少超过最大匹配窗口，避免截断最后的输出。两通道不能是先后录制后拼接。输入参考也必须来自同一个原始时钟下的同步采集。
5. 保存原始 WAV、完整 CLI 参数、JSON、实验条件和文件哈希以便复现；原始录音由用户自己保管，本工具不复制或导出音频。

**仅当物理 reference setup 成立时，结果才是该参考起点到耳机物理输出的端到端延迟。** 若 channel0 来自链路中途、数字回环或软件信号，它只能测到该参考与输出之间的相对延迟，不能代表完整端到端。麦克风距离产生的声传播延迟、采集通道偏差和模拟滤波也可能包含在差值中。文件内容本身无法证明接线和参考位置正确。

## 使用

```powershell
python -B tools/audio/analyze_latency.py user-recorded-stereo.wav
python -B tools/audio/analyze_latency.py user-recorded-stereo.wav --threshold 0.2 --min-separation-ms 500 --max-delay-ms 200 > latency-report.json
```

`--window-ms` 是 `--max-delay-ms` 的同义参数。WAV 必须是 RIFF little-endian、恰好双声道、16/24/32-bit 有符号 integer PCM；也支持有效位宽等于容器位宽的 WAVE_FORMAT_EXTENSIBLE PCM。IEEE float WAVE（包括 extensible float）会被**明确拒绝**，不把 float 数据误当 32-bit integer。RIFX/RF64、压缩 WAV、8-bit PCM、有效位宽不同于容器位宽均不支持；请用户在保留原始录音的情况下另行转换，并记录转换过程。

成功时 stdout 只有 JSON；配置/录音错误退出码为 2，错误 JSON 写 stderr。帮助及 argparse 参数语法错误遵从 argparse 行为。空文件/空 data、无参考或无接收脉冲、单声道、损坏 WAV、非有限数值和错误阈值均拒绝。如果两路都有脉冲但没有任何窗口内正延迟匹配，则返回 `matched_count: 0`、未匹配率 1、延迟统计 `null` 并给出警告；这不是有效延迟结论。

### 检测与匹配定义

- 整数样本除以 `2^(bits-1)` 归一化，阈值范围为 `0 < threshold <= 1`，默认 0.2；不做逐通道峰值归一化或自动降阈值。正负脉冲都按**绝对幅度**检测，所以允许反相。满量程正轨值略小于 1，阈值 1 只可能检测到负轨值，通常不要使用这个极限值。
- 事件时间是首次 `abs(sample) >= threshold` 的采样位置。某个事件起点之后，小于 `min-separation-ms` 的后续上穿被合并，适合稀疏脉冲和短 burst；持续高电平不会周期性重复计数。不是互相关、包络拟合或亚采样插值。模拟噪声、振铃、多径及不同幅度造成的 threshold time-walk 会影响结果，应在已知静音噪声上方选定固定阈值并验证。
- 严格只匹配 `0 < received_sample - reference_sample <= floor(window_ms * sample_rate / 1000)`。对每个参考按时间先后选择窗口中的**第一个后续接收事件**；每个接收事件最多使用一次，零/负延迟不匹配，也不会把两路事件简单按序号 zip 对齐。时间分离向上取整到样本，窗口向下取整；JSON 给出实际采样数。
- 强制 `max-delay-ms < min-separation-ms`，默认 200ms < 500ms；在真实延迟不超过该窗口、参考为稀疏独立激励、没有伪脉冲的前提下，丢失的接收脉冲不会拿下一个参考对应的输出顶替。窗口之外的接收事件保留给之后的参考判断。必须先通过实验给出可靠的最大延迟上界，不要为了提高匹配率而任意放宽窗口。
- **局限**：只有时间与阈值信息，没有脉冲身份编码，无法区分窗口内噪声伪事件、异常超长延迟恰好落入后续窗口等情形。若真实延迟可能大于窗口、脉冲间隔过短、burst 太长或信噪比不足，必须重新设计激励/间隔/采集条件，不能将匹配统计直接认作物理真值。

### JSON 字段

- `sample_rate_hz`、`sample_frames`（每通道样本数）、`sample_values`（双通道样本总数）、`pcm_bits`、`duration_seconds`。
- `config`：阈值、分离时间、最大正延迟及其整数采样数、通道定义、检测/匹配方法与百分位方法。
- `latency_ms.median/p95/p99/max`：只针对成功匹配对，numpy `linear` 百分位；小样本的 p99 不代表统计置信保证。采样分辨率为 `1000 / sample_rate_hz` ms，报告不声称亚采样精度。
- `reference_event_count`、`received_event_count`、`matched_count`；`unmatched_reference_rate = (reference_event_count - matched_count) / reference_event_count`，接收端也有独立未匹配数和比例，范围 0–1，乘 100 得百分比。未匹配参考可能来自丢脉冲、窗口外延迟或尾部截断等，**不是被测系统丢包率**。仅匹配对的分位数可能低估超窗/丢失事件，须同时查看未匹配率。
- `clipping`：逐通道 `rail_sample_count` 与 `rail_sample_rate`，统计整数正/负轨值（可能削波，但无法从整数文件证明其成因）。一旦出现即警告；零轨值也不能保证前级没有模拟削波。不自动改变音量或增益。
- `measurement_note` 和 `warnings`：物理参考成立条件及测量质量提醒。报告不包含任何原始音频样本或音频片段。

## 合成回归测试

```powershell
python -B -m unittest discover -s tools/audio -p test_analyze_latency.py -v
```

测试在内存中生成双声道 WAV，不使用系统 TEMP、不写临时音频；`-B` 避免在仓库生成 `__pycache__`。固定随机种子覆盖已知延迟/抖动/丢脉冲、反相短 burst、16/24/32-bit 和 24-bit 符号扩展、48kHz 采样分辨率、静音/零/噪声、削波、错误阈值/格式和 CLI JSON。该测试只能验证分析算法，不能代替真实耳机物理接线实验。
