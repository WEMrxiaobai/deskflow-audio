#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
"""离线分析用户录制的双声道物理延迟 WAV；只读输入，JSON 写 stdout。"""

import argparse
import json
import math
from pathlib import Path
import struct
import sys

import numpy as np


class AnalysisError(ValueError):
    """录音、格式或测量配置不适合分析。"""


PCM_GUID = bytes.fromhex("0100000000001000800000aa00389b71")
FLOAT_GUID = bytes.fromhex("0300000000001000800000aa00389b71")
MEASUREMENT_NOTE = (
    "仅当 channel0 是被测链路起点的真实物理参考、channel1 是耳机物理输出，"
    "且两通道同步采集并校准采集通道偏差时，差值才代表该参考起点至耳机输出的端到端延迟。"
    "文件本身不能验证接线；软件时间戳、数字回环或链路中途参考不能证明完整端到端延迟。"
)


def read_pcm_wav(path):
    """读 RIFF little-endian PCM（含等有效位宽的 extensible PCM），返回有符号整数。"""
    raw = Path(path).read_bytes()
    if len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise AnalysisError("需要 RIFF little-endian WAV（不支持 RIFX/RF64）。")
    end = struct.unpack_from("<I", raw, 4)[0] + 8
    if end < 12 or end > len(raw):
        raise AnalysisError("WAV RIFF 长度错误或文件截断。")
    fmt = None
    payload = None
    offset = 12
    while offset < end:
        if offset + 8 > end:
            raise AnalysisError("WAV chunk 头截断。")
        chunk_id = raw[offset:offset + 4]
        length = struct.unpack_from("<I", raw, offset + 4)[0]
        start = offset + 8
        stop = start + length
        if stop + (length & 1) > end:
            raise AnalysisError("WAV chunk 数据或填充截断。")
        if chunk_id == b"fmt ":
            if fmt is not None:
                raise AnalysisError("不支持多个 fmt chunk。")
            fmt = raw[start:stop]
        elif chunk_id == b"data":
            if payload is not None:
                raise AnalysisError("不支持多个 data chunk。")
            payload = memoryview(raw)[start:stop]
        offset = stop + (length & 1)
    if fmt is None or len(fmt) < 16 or payload is None:
        raise AnalysisError("WAV 缺少有效 fmt/data chunk。")
    tag, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", fmt)
    if tag == 3:
        raise AnalysisError("尚不支持 IEEE float WAVE；请显式转换为 16/24/32-bit integer PCM。")
    if tag == 0xFFFE:
        if len(fmt) < 40:
            raise AnalysisError("WAVE_FORMAT_EXTENSIBLE fmt 截断。")
        extension_size, valid_bits = struct.unpack_from("<HH", fmt, 16)
        if extension_size < 22 or len(fmt) < 18 + extension_size:
            raise AnalysisError("WAVE_FORMAT_EXTENSIBLE 扩展长度错误。")
        subtype = fmt[24:40]
        if subtype == FLOAT_GUID:
            raise AnalysisError("尚不支持 IEEE float WAVE（extensible）。")
        if subtype != PCM_GUID:
            raise AnalysisError("仅支持 integer PCM WAVE subtype。")
        if valid_bits != bits:
            raise AnalysisError("尚不支持有效位宽与容器位宽不同的 extensible PCM。")
    elif tag != 1:
        raise AnalysisError("仅支持未压缩 integer PCM WAV。")
    if channels != 2:
        raise AnalysisError("需要恰好双声道：channel0 参考，channel1 耳机物理输出。")
    if bits not in (16, 24, 32):
        raise AnalysisError("仅支持 16/24/32-bit integer PCM。")
    expected_align = channels * (bits // 8)
    if rate <= 0 or align != expected_align or byte_rate != rate * align:
        raise AnalysisError("WAV 采样率、block-align 或 byte-rate 无效。")
    if len(payload) == 0:
        raise AnalysisError("空录音：data 中没有采样帧。")
    if len(payload) % align:
        raise AnalysisError("WAV data 长度不是完整采样帧。")
    if bits == 24:
        octets = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        samples = octets[:, 0] | (octets[:, 1] << 8) | (octets[:, 2] << 16)
        samples = (samples ^ 0x800000) - 0x800000
    else:
        samples = np.frombuffer(payload, dtype="<i2" if bits == 16 else "<i4")
    return samples.reshape(-1, 2), rate, bits


def detect_events(signal, threshold, min_separation_samples):
    """绝对幅度首次上穿阈值；在上次已接受起点后的 refractory 内合并短 burst。"""
    above = np.abs(signal) >= threshold
    rising = above.copy()
    rising[1:] &= ~above[:-1]
    candidates = np.flatnonzero(rising)
    events = []
    for candidate in candidates:
        sample = int(candidate)
        if not events or sample - events[-1] >= min_separation_samples:
            events.append(sample)
    return events


def match_events(reference, received, max_delay_samples):
    """每个参考配第一个严格在后的接收事件，窗口内匹配且不重复使用。"""
    matches = []
    cursor = 0
    for ref in reference:
        while cursor < len(received) and received[cursor] <= ref:
            cursor += 1
        if cursor < len(received) and received[cursor] - ref <= max_delay_samples:
            matches.append((ref, received[cursor]))
            cursor += 1
        # 越窗的接收事件保留给之后的参考，绝不靠事件序号 zip 对齐。
    return matches


def analyze(path, threshold=0.2, min_separation_ms=500.0, max_delay_ms=200.0):
    """返回不含音频样本的可 JSON 序列化测量报告。"""
    for name, value in (("threshold", threshold), ("min_separation_ms", min_separation_ms),
                        ("max_delay_ms", max_delay_ms)):
        if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
            raise AnalysisError(f"{name} 必须为有限数值。")
    if not 0 < threshold <= 1:
        raise AnalysisError("threshold 必须满足 0 < threshold <= 1（归一化满量程）。")
    if min_separation_ms <= 0 or max_delay_ms <= 0:
        raise AnalysisError("min-separation-ms 和 max-delay-ms 必须大于零。")
    if max_delay_ms >= min_separation_ms:
        raise AnalysisError("max-delay-ms 必须小于 min-separation-ms，以避免丢脉冲后错配下一个。")
    samples, rate, bits = read_pcm_wav(path)
    separation_float = min_separation_ms * (rate / 1000.0)
    window_float = max_delay_ms * (rate / 1000.0)
    if not math.isfinite(separation_float) or not math.isfinite(window_float):
        raise AnalysisError("时间配置过大，无法换算为有限采样数。")
    separation = math.ceil(separation_float)
    window = math.floor(window_float)
    if window < 1:
        raise AnalysisError("max-delay-ms 小于一个采样周期，不能匹配严格正延迟。")
    full_scale = 1 << (bits - 1)
    events = []
    clipping = []
    for channel in range(2):
        integers = samples[:, channel]
        count = int(np.count_nonzero((integers == -full_scale) | (integers == full_scale - 1)))
        clipping.append({"channel": channel, "rail_sample_count": count,
                         "rail_sample_rate": count / len(samples)})
        signal = integers.astype(np.float64) / full_scale
        events.append(detect_events(signal, threshold, separation))
    if not events[0]:
        raise AnalysisError("channel0 未检测到参考脉冲：请检查接线、threshold 和录音。")
    if not events[1]:
        raise AnalysisError("channel1 未检测到接收脉冲：请检查耳机物理输出、threshold 和录音。")
    pairs = match_events(events[0], events[1], window)
    delays = np.array([rec - ref for ref, rec in pairs], dtype=np.float64) * 1000.0 / rate
    stats = {"median": None, "p95": None, "p99": None, "max": None}
    if len(delays):
        median, p95, p99 = np.percentile(delays, [50, 95, 99], method="linear")
        stats = {"median": float(median), "p95": float(p95), "p99": float(p99),
                 "max": float(np.max(delays))}
    warnings = []
    if any(item["rail_sample_count"] for item in clipping):
        warnings.append("检测到整数满量程轨值（可能削波）；建议降低采集增益后重新测量，工具不更改音量。")
    if not pairs:
        warnings.append("没有窗口内的严格正延迟匹配；统计为 null，不能作为有效延迟结果。")
    if min_separation_ms < 500:
        warnings.append("建议事件最小分离不少于 500ms，并使实际激励间隔大于分离时间与延迟抖动之和。")
    reference_count, received_count = map(len, events)
    return {
        "schema_version": 1,
        "measurement": "physical_reference_to_headphone_threshold_onset",
        "measurement_note": MEASUREMENT_NOTE,
        "sample_rate_hz": rate,
        "sample_frames": len(samples),
        "sample_values": int(samples.size),
        "channels": 2,
        "pcm_bits": bits,
        "duration_seconds": len(samples) / rate,
        "config": {"threshold": threshold, "min_separation_ms": min_separation_ms,
                   "max_delay_ms": max_delay_ms, "min_separation_samples": separation,
                   "max_delay_samples": window, "reference_channel": 0, "received_channel": 1,
                   "polarity": "absolute", "event_time": "first_threshold_crossing",
                   "matching": "first_strictly_next_within_window_no_reuse",
                   "percentile_method": "linear"},
        "reference_event_count": reference_count,
        "received_event_count": received_count,
        "matched_count": len(pairs),
        "unmatched_reference_count": reference_count - len(pairs),
        "unmatched_reference_rate": (reference_count - len(pairs)) / reference_count,
        "unmatched_received_count": received_count - len(pairs),
        "unmatched_received_rate": (received_count - len(pairs)) / received_count,
        "latency_ms": stats,
        "clipping": clipping,
        "warnings": warnings,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", help="用户已明确录制的双声道 integer PCM WAV（工具不录音）")
    parser.add_argument("--threshold", type=float, default=0.2, help="绝对归一化幅度，范围 (0,1]；默认 0.2")
    parser.add_argument("--min-separation-ms", type=float, default=500.0, help="同通道事件起点最小分离；默认 500ms")
    parser.add_argument("--max-delay-ms", "--window-ms", dest="max_delay_ms", type=float, default=200.0,
                        help="最大正匹配延迟（包含上界），必须小于事件分离；默认 200ms")
    args = parser.parse_args(argv)
    try:
        report = analyze(args.wav, args.threshold, args.min_separation_ms, args.max_delay_ms)
    except (AnalysisError, OSError) as exc:
        print(json.dumps({"error": str(exc)}, ensure_ascii=True, allow_nan=False), file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=True, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
