"""Frame screening must expose short bursts, channel defects and partial tails."""
import json
import unittest

import numpy as np

from evaluate_encoder_quality import (FRAME_SAMPLES, ROOT, check_signature, frame_diagnostics,
                                      frame_pairs, validate_build_metadata)


class EncoderQualityTests(unittest.TestCase):
    def test_final_partial_frame_is_included_and_not_diluted(self):
        source = np.full((FRAME_SAMPLES + 1, 1), 1000.0)
        output = source.copy()
        output[-1] = 32767
        rows = frame_diagnostics(source, output)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["error_squared_sum"], 0)
        self.assertEqual(rows[1]["frames"], 1)
        self.assertEqual(rows[1]["error_squared_sum"], 31767 ** 2)
        self.assertTrue(rows[1]["severe"])
        self.assertEqual(rows[1]["unexpected_full_scale"], 1)

    def test_loud_good_channel_cannot_hide_bad_quiet_channel(self):
        source = np.empty((FRAME_SAMPLES, 2))
        source[:, 0] = 30000
        source[:, 1] = 100
        output = source.copy()
        output[:, 1] += 200
        rows = frame_diagnostics(source, output)
        self.assertFalse(rows[0]["severe"])
        self.assertTrue(rows[1]["error_exceeds_signal"])
        self.assertEqual(rows[1]["error_rms"], 200)

    def test_exact_full_scale_and_silence_are_not_clip_bursts(self):
        source = np.array([[-32768, 0], [32767, 0]], dtype=np.int16)
        rows = frame_diagnostics(source, source)
        self.assertEqual(rows[0]["source_full_scale"], 2)
        self.assertEqual(rows[0]["unexpected_full_scale"], 0)
        self.assertTrue(all(not row["severe"] for row in rows))
        self.assertTrue(all(row["snr_db"] is None for row in rows))
        json.dumps(rows, allow_nan=False)

    def test_sustained_new_full_scale_output_is_exposed(self):
        source = np.full((100, 1), 1000.0)
        output = source.copy()
        output[:4] = 32767
        row = frame_diagnostics(source, output)[0]
        self.assertTrue(row["clip_burst"])
        self.assertEqual(row["unexpected_full_scale"], 4)

    def test_regression_floor_ignores_tiny_errors_but_not_audible_growth(self):
        source = np.full((100, 1), 1000.0)
        baseline = frame_diagnostics(source, source)
        tiny = frame_pairs(baseline, frame_diagnostics(source, source + 1))[0]
        large = frame_pairs(baseline, frame_diagnostics(source, source + 200))[0]
        self.assertFalse(tiny["error_regression"])
        self.assertTrue(large["error_regression"])
        self.assertIsNone(large["sse_ratio"])
        json.dumps(large, allow_nan=False)

    def test_resolved_and_introduced_frames_are_distinct(self):
        source = np.full((100, 1), 1000.0)
        clean = frame_diagnostics(source, source)
        broken = frame_diagnostics(source, np.full_like(source, 32767))
        fixed = frame_pairs(broken, clean)[0]
        introduced = frame_pairs(clean, broken)[0]
        self.assertTrue(fixed["resolved_severe"])
        self.assertFalse(fixed["introduced_severe"])
        self.assertTrue(introduced["introduced_severe"])
        self.assertFalse(introduced["resolved_severe"])

    def test_timeline_mismatches_fail(self):
        with self.assertRaises(ValueError):
            frame_diagnostics(np.zeros((20, 1)), np.zeros((19, 1)))
        with self.assertRaises(ValueError):
            frame_diagnostics(np.zeros((20, 1)), np.zeros((20, 2)))
        with self.assertRaises(ValueError):
            frame_pairs(frame_diagnostics(np.zeros((20, 1)), np.zeros((20, 1))),
                        frame_diagnostics(np.zeros((19, 1)), np.zeros((19, 1))))

    def test_frozen_baseline_output_changes_are_rejected(self):
        row = dict(stored_frames=20, stored_rate=44100, channels=1, encoded_bytes=40,
                   encoded_fnv64="a", decoded_fnv64="b")
        check_signature(row, row.copy())
        with self.assertRaises(ValueError):
            check_signature(row | {"decoded_fnv64": "c"}, row)

    def test_build_metadata_must_match_measured_sources_and_executables(self):
        hashes = {name: {"path": str(ROOT / path), "sha256": digest} for name, path, digest in (
            ("candidate_header", "src/rg_rgs.h", "a" * 64),
            ("candidate_executable", "build/candidate.exe", "b" * 64),
            ("adapter_source", "benchmarks/codec_adapter.c", "c" * 64))}
        build = {"sha256": {row["path"]: row["sha256"] for row in hashes.values()}}
        validate_build_metadata(build, hashes)
        build["sha256"][hashes["candidate_executable"]["path"]] = "d" * 64
        with self.assertRaisesRegex(ValueError, "candidate_executable"):
            validate_build_metadata(build, hashes)

    def test_relocated_frozen_header_must_remain_byte_identical(self):
        hashes = {name: {"path": str(ROOT / path), "sha256": digest} for name, path, digest in (
            ("candidate_header", "new_header.h", "a" * 64),
            ("candidate_executable", "build/candidate.exe", "b" * 64),
            ("adapter_source", "benchmarks/codec_adapter.c", "c" * 64))}
        build = {"sha256": {"old_header.h": "a" * 64, "build/candidate.exe": "b" * 64,
                            "benchmarks/codec_adapter.c": "c" * 64}}
        validate_build_metadata(build, hashes)
        hashes["candidate_header"]["sha256"] = "d" * 64
        with self.assertRaisesRegex(ValueError, "candidate_header"):
            validate_build_metadata(build, hashes)


if __name__ == "__main__":
    unittest.main()
