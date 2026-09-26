"""Deterministic correctness checks for benchmark metrics and aggregation."""
import unittest
from unittest.mock import patch
import os
from pathlib import Path
import subprocess
import numpy as np
from run_benchmarks import aggregate, category_summary, native, quality_metrics


class HarnessTests(unittest.TestCase):
    def test_configured_audio_libraries_take_precedence(self):
        custom = str(Path("build/custom-audio-libraries").resolve())
        with patch.dict(os.environ, {"RG_AUDIO_DEPS_DIR": custom}):
            with patch("run_benchmarks.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "{}", "")) as run:
                self.assertEqual(native(Path("unused-adapter")), {})
                self.assertEqual(run.call_args.kwargs["env"]["PATH"].split(os.pathsep)[0], str(Path(custom) / "bin"))

    def test_exact_and_silence_are_json_safe(self):
        import json
        values = np.zeros((1000, 2))
        result = quality_metrics(values, values, 44100)
        self.assertTrue(result["exact_pcm"])
        self.assertIsNone(result["snr_db"])
        self.assertEqual(result["log_spectral_rms_db"], 0)
        json.dumps(result, allow_nan=False)

    def test_known_snr_and_truncated_timeline(self):
        values = np.full((1000, 1), 1000.0)
        result = quality_metrics(values, values[:999] + 100, 44100)
        self.assertAlmostEqual(result["snr_db"], 20)
        self.assertFalse(result["exact_pcm"])
        self.assertEqual(result["compared_frames"], 999)
        self.assertEqual(result["max_abs_error"], 100)

    def test_channels_rejected(self):
        with self.assertRaises(ValueError):
            quality_metrics(np.zeros((20, 1)), np.zeros((20, 2)), 44100)

    def test_categories_sum_duration_and_medians_not_mean_rates(self):
        rows = []
        for asset, frames in (("short", 100), ("long", 900)):
            for trial, elapsed in enumerate((1, 2, 9)):
                rows.append(dict(asset=asset, category="sfx", lane="normalized", revision="candidate",
                                 codec="rgs", quality="medium", source_frames=frames, source_rate=100,
                                 stored_frames=frames, stored_rate=100, channels=1, encoded_bytes=frames,
                                 encode_ms=elapsed, decode_ms=elapsed, trial=trial))
        files = aggregate(rows)
        self.assertEqual(files[0]["decode_median_ms"], 2)
        category = category_summary(files)[0]
        self.assertEqual(category["duration_seconds"], 10)
        self.assertEqual(category["decode_sum_median_ms"], 4)
        self.assertEqual(category["kbps"], 0.8)


if __name__ == "__main__":
    unittest.main()
