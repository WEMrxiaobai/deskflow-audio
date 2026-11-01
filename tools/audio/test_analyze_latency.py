# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
"""合成双声道 WAV 测试；全部在内存中，不录音、不写出音频。"""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import struct
import unittest
from unittest.mock import patch

import numpy as np

if __package__:
    from . import analyze_latency as latency
else:
    import analyze_latency as latency


def wav_bytes(samples, bits=16, channels=2, tag=1, extensible=False):
    """从归一化数值生成已知格式的小型 RIFF WAV（仅用于测试）。"""
    scale = 1 << (bits - 1)
    integers = np.clip(np.rint(np.asarray(samples) * scale), -scale, scale - 1).astype(np.int64)
    if bits == 24:
        flat = integers.reshape(-1)
        octets = np.column_stack((flat & 255, (flat >> 8) & 255, (flat >> 16) & 255))
        payload = octets.astype(np.uint8).tobytes()
    else:
        payload = integers.astype("<i2" if bits == 16 else "<i4").tobytes()
    align = channels * (bits // 8)
    fmt = struct.pack("<HHIIHH", 0xFFFE if extensible else tag, channels, 1000,
                      1000 * align, align, bits)
    if extensible:
        subtype = latency.FLOAT_GUID if tag == 3 else latency.PCM_GUID
        fmt += struct.pack("<HHI", 22, bits, 3) + subtype
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(payload)) + payload
    if len(payload) & 1:
        chunks += b"\x00"
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks


def synthetic(delays=(12, 15, 18, 21, 24), dropped=(), invert=False, noise=0.0, burst=False):
    references = [100 + 800 * index for index in range(len(delays))]
    samples = np.random.default_rng(391).uniform(-noise, noise, (references[-1] + 400, 2))
    # 第一个样本超过阈值；burst 有多个正负波瓣，不能被重复计数。
    shape = np.array([0.75, 0.0, -0.65, 0.0, 0.5]) if burst else np.array([0.75])
    for index, (ref, delay) in enumerate(zip(references, delays)):
        samples[ref:ref + len(shape), 0] += shape
        if index not in dropped:
            received = ref + delay
            samples[received:received + len(shape), 1] += -shape if invert else shape
    return samples


class LatencyTests(unittest.TestCase):
    def analyze_bytes(self, raw, **kwargs):
        with patch.object(latency.Path, "read_bytes", return_value=raw):
            return latency.analyze("synthetic.wav", **kwargs)

    def test_known_delays_jitter_percentiles(self):
        report = self.analyze_bytes(wav_bytes(synthetic()))
        self.assertEqual(report["matched_count"], 5)
        self.assertEqual(report["unmatched_reference_rate"], 0)
        self.assertEqual(report["latency_ms"]["median"], 18)
        self.assertAlmostEqual(report["latency_ms"]["p95"], 23.4)
        self.assertAlmostEqual(report["latency_ms"]["p99"], 23.88)
        self.assertEqual(report["latency_ms"]["max"], 24)
        self.assertEqual(report["sample_rate_hz"], 1000)
        self.assertEqual(report["sample_frames"], 3700)
        self.assertEqual(report["sample_values"], 7400)
        self.assertEqual(report["config"]["max_delay_ms"], 200)
        self.assertEqual(report["config"]["min_separation_ms"], 500)
        self.assertIn("物理参考", report["measurement_note"])
        json.dumps(report, allow_nan=False)

    def test_dropped_pulse_does_not_shift_later_matches(self):
        report = self.analyze_bytes(wav_bytes(synthetic(dropped=(1, 3))))
        self.assertEqual(report["matched_count"], 3)
        self.assertEqual(report["received_event_count"], 3)
        self.assertEqual(report["unmatched_reference_count"], 2)
        self.assertEqual(report["unmatched_reference_rate"], 0.4)
        self.assertEqual(report["latency_ms"]["median"], 18)
        self.assertEqual(report["latency_ms"]["max"], 24)

    def test_inverted_short_burst_and_subthreshold_noise(self):
        report = self.analyze_bytes(wav_bytes(synthetic(invert=True, noise=0.025, burst=True), bits=24))
        self.assertEqual(report["reference_event_count"], 5)
        self.assertEqual(report["received_event_count"], 5)
        self.assertEqual(report["matched_count"], 5)
        self.assertEqual(report["latency_ms"]["median"], 18)

    def test_all_supported_bit_depths_and_negative_polarity(self):
        for bits in (16, 24, 32):
            with self.subTest(bits=bits):
                report = self.analyze_bytes(wav_bytes(-synthetic(), bits=bits))
                self.assertEqual(report["pcm_bits"], bits)
                self.assertEqual(report["matched_count"], 5)
                self.assertEqual(report["latency_ms"]["median"], 18)

    def test_24bit_sign_extension_and_little_endian(self):
        values = np.array([[-1, 1 - 2 ** -23], [-0.5, 0.5], [-2 ** -23, 2 ** -23]])
        with patch.object(latency.Path, "read_bytes", return_value=wav_bytes(values, bits=24)):
            samples, rate, bits = latency.read_pcm_wav("memory.wav")
        np.testing.assert_array_equal(samples, [[-8388608, 8388607], [-4194304, 4194304], [-1, 1]])
        self.assertEqual((rate, bits), (1000, 24))

    def test_extensible_pcm(self):
        for bits in (16, 24, 32):
            with self.subTest(bits=bits):
                report = self.analyze_bytes(wav_bytes(synthetic(), bits=bits, extensible=True))
                self.assertEqual(report["matched_count"], 5)

    def test_float_is_explicitly_rejected(self):
        for extensible in (False, True):
            with self.subTest(extensible=extensible):
                with self.assertRaisesRegex(latency.AnalysisError, "float"):
                    self.analyze_bytes(wav_bytes(synthetic(), bits=32, tag=3, extensible=extensible))

    def test_empty_recording_rejected(self):
        with self.assertRaisesRegex(latency.AnalysisError, "空录音"):
            self.analyze_bytes(wav_bytes(np.empty((0, 2))))

    def test_zero_noise_and_missing_channel_events_rejected(self):
        for samples in (np.zeros((2000, 2)),
                        np.random.default_rng(1).uniform(-0.02, 0.02, (2000, 2)),
                        synthetic(dropped=range(5))):
            with self.subTest():
                with self.assertRaisesRegex(latency.AnalysisError, "未检测到"):
                    self.analyze_bytes(wav_bytes(samples))

    def test_mono_rejected(self):
        with self.assertRaisesRegex(latency.AnalysisError, "双声道"):
            self.analyze_bytes(wav_bytes(np.zeros((2000, 1)), channels=1))

    def test_invalid_threshold_rejected(self):
        for threshold in (0, -0.1, 1.1, float("nan"), float("inf"), True, "0.2"):
            with self.subTest(threshold=threshold):
                with self.assertRaises(latency.AnalysisError):
                    self.analyze_bytes(wav_bytes(synthetic()), threshold=threshold)

    def test_threshold_one_is_valid_and_detects_negative_rail(self):
        samples = np.zeros((800, 2))
        samples[100, 0] = -1
        samples[120, 1] = -1
        report = self.analyze_bytes(wav_bytes(samples), threshold=1)
        self.assertEqual(report["latency_ms"]["median"], 20)

    def test_invalid_or_unsafe_window_rejected(self):
        for config in ({"max_delay_ms": 0}, {"max_delay_ms": -1},
                       {"min_separation_ms": 0}, {"min_separation_ms": float("nan")},
                       {"max_delay_ms": float("inf")}, {"max_delay_ms": 500},
                       {"max_delay_ms": 600}, {"max_delay_ms": 0.1}):
            with self.subTest(config=config):
                with self.assertRaises(latency.AnalysisError):
                    self.analyze_bytes(wav_bytes(synthetic()), **config)

    def test_first_next_no_reuse_and_inclusive_window(self):
        self.assertEqual(latency.match_events([100, 900, 1700], [90, 100, 120, 135, 1100, 1950], 200),
                         [(100, 120), (900, 1100)])
        self.assertEqual(latency.match_events([100, 900], [950], 200), [(900, 950)])
        self.assertEqual(latency.match_events([100, 110], [120], 200), [(100, 120)])
        self.assertEqual(latency.match_events([100], [100], 200), [])

    def test_out_of_window_all_unmatched_stats_null(self):
        report = self.analyze_bytes(wav_bytes(synthetic(delays=(250, 250, 250))))
        self.assertEqual(report["matched_count"], 0)
        self.assertEqual(report["unmatched_reference_rate"], 1)
        self.assertEqual(report["unmatched_received_rate"], 1)
        self.assertTrue(all(value is None for value in report["latency_ms"].values()))
        self.assertTrue(report["warnings"])

    def test_clipping_both_integer_rails(self):
        for bits in (16, 24, 32):
            samples = synthetic()
            samples[100:102, 0] = [-1, 1]
            samples[112:114, 1] = [1, -1]
            report = self.analyze_bytes(wav_bytes(samples, bits=bits))
            with self.subTest(bits=bits):
                self.assertEqual([item["rail_sample_count"] for item in report["clipping"]], [2, 2])
                self.assertAlmostEqual(report["clipping"][0]["rail_sample_rate"], 2 / len(samples))
                self.assertTrue(report["warnings"])

    def test_truncated_or_invalid_wav(self):
        valid = wav_bytes(synthetic())
        damaged = [b"", b"not wave", b"RIFX" + valid[4:], valid[:-1],
                   valid[:4] + struct.pack("<I", 0) + valid[8:]]
        for raw in damaged:
            with self.subTest(length=len(raw)):
                with self.assertRaises(latency.AnalysisError):
                    self.analyze_bytes(raw)

    def test_invalid_block_alignment(self):
        raw = bytearray(wav_bytes(synthetic()))
        struct.pack_into("<H", raw, 32, 3)
        with self.assertRaisesRegex(latency.AnalysisError, "block-align"):
            self.analyze_bytes(bytes(raw))

    def test_incomplete_sample_frame_rejected(self):
        raw = bytearray(wav_bytes(synthetic()))
        # 缩短 data 两字节并同步 RIFF 长度，保证错误在采样帧校验处被检出。
        struct.pack_into("<I", raw, 40, len(raw) - 46)
        raw = raw[:-2]
        struct.pack_into("<I", raw, 4, len(raw) - 8)
        with self.assertRaisesRegex(latency.AnalysisError, "完整采样帧"):
            self.analyze_bytes(bytes(raw))

    def test_48000hz_sample_resolution(self):
        samples = np.zeros((10000, 2))
        samples[100, 0] = 0.8
        samples[1334, 1] = -0.8
        raw = bytearray(wav_bytes(samples, bits=32))
        struct.pack_into("<II", raw, 24, 48000, 48000 * 8)
        report = self.analyze_bytes(bytes(raw))
        self.assertEqual(report["sample_rate_hz"], 48000)
        self.assertAlmostEqual(report["latency_ms"]["median"], 1234 * 1000 / 48000)

    def test_cli_stdout_is_json_without_audio(self):
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(latency.Path, "read_bytes", return_value=wav_bytes(synthetic())), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            code = latency.main(["synthetic.wav", "--threshold", "0.2", "--window-ms", "100"])
        self.assertEqual(code, 0)
        report = json.loads(stdout.getvalue())
        self.assertEqual(report["matched_count"], 5)
        self.assertEqual(report["config"]["max_delay_ms"], 100)
        self.assertNotIn("samples", report)
        self.assertEqual(stderr.getvalue(), "")

    def test_cli_error_only_on_stderr(self):
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = latency.main(["synthetic.wav", "--threshold", "nan"])
        self.assertEqual(code, 2)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("error", json.loads(stderr.getvalue()))

    def test_cli_missing_file_is_clean_error(self):
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(latency.Path, "read_bytes", side_effect=FileNotFoundError("missing")), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            code = latency.main(["missing.wav"])
        self.assertEqual(code, 2)
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(json.loads(stderr.getvalue()), {"error": "missing"})


if __name__ == "__main__":
    unittest.main()
