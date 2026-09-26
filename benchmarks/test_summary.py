"""Publication validation must reject incomplete or internally inconsistent runs."""
import copy
import unittest
from summarize_review import validate


def fixture():
    manifest = {"samples": [{"relative_path": "sfx.wav", "category": "oculus_audio_pack", "samplerate": 48000, "sha256": "a" * 64}]}
    cases = [("normalized", revision, "rgs", quality) for revision in ("baseline", "candidate") for quality in ("high", "medium", "low")]
    cases += [("normalized", "reference", codec, "default") for codec in ("qoa", "ima", "pcm")]
    cases += [("legacy_import", "baseline", "rgs", quality) for quality in ("high", "medium", "low")]
    files, raw = [], []
    for lane, revision, codec, quality in cases:
        row = {"asset": "sfx.wav", "category": "oculus_audio_pack", "lane": lane, "revision": revision,
               "codec": codec, "quality": quality, "source_frames": 100, "source_rate": 44100,
               "stored_frames": 100, "stored_rate": 44100, "channels": 1, "encoded_bytes": 100,
               "source_sha256": "a" * 64, "prepared_sha256": "b" * 64,
               "encoded_fnv64": "c" * 16, "decoded_fnv64": "d" * 16,
               "encoded_sha256": "c" * 64, "decoded_sha256": "d" * 64,
               "quality_metrics": {"snr_db": 20, "exact_pcm": False}}
        files.append(row | {"trials": 7, "encode_median_ms": 4, "decode_median_ms": 4})
        raw.extend(row | {"trial": trial, "encode_ms": trial + 1, "decode_ms": trial + 1} for trial in range(7))
    pairs = [{"asset": "sfx.wav", "quality": quality, "encoded_identical": True, "decoded_identical": True}
             for quality in ("high", "medium", "low")]
    report = {"metadata": {"trials": 7, "argv": ["--legacy-import"]}, "errors": [], "per_file": files, "baseline_candidate": pairs}
    return report, manifest, raw


class SummaryValidationTests(unittest.TestCase):
    def test_complete_paired_run(self):
        report, manifest, raw = fixture()
        self.assertEqual(len(validate(report, manifest, raw)), 12)

    def test_duplicate_summary_cannot_hide_in_coverage_set(self):
        report, manifest, raw = fixture()
        report["per_file"].append(copy.deepcopy(report["per_file"][0]))
        with self.assertRaisesRegex(ValueError, "Duplicate summary"):
            validate(report, manifest, raw)

    def test_missing_pair_cannot_support_identical_claim(self):
        report, manifest, raw = fixture()
        report["baseline_candidate"] = []
        with self.assertRaisesRegex(ValueError, "before/after"):
            validate(report, manifest, raw)

    def test_missing_requested_legacy_case_rejected(self):
        report, manifest, raw = fixture()
        report["per_file"] = [row for row in report["per_file"] if row["lane"] != "legacy_import"]
        with self.assertRaisesRegex(ValueError, "legacy"):
            validate(report, manifest, raw)

    def test_duplicate_trial_cannot_substitute_for_missing_trial(self):
        report, manifest, raw = fixture()
        raw[1]["trial"] = 0
        with self.assertRaisesRegex(ValueError, "Duplicate raw"):
            validate(report, manifest, raw)

    def test_changed_output_rejected(self):
        report, manifest, raw = fixture()
        raw[1]["decoded_fnv64"] = "e" * 16
        with self.assertRaisesRegex(ValueError, "between trials"):
            validate(report, manifest, raw)

    def test_incorrect_summary_timing_rejected(self):
        report, manifest, raw = fixture()
        report["per_file"][0]["decode_median_ms"] = 0.01
        with self.assertRaisesRegex(ValueError, "Timing summary"):
            validate(report, manifest, raw)

    def test_independent_hash_pairing_rejects_false_identical_flags(self):
        report, manifest, raw = fixture()
        for row in report["per_file"] + raw:
            if row["revision"] == "candidate" and row["quality"] == "high":
                row["decoded_sha256"] = "e" * 64
        with self.assertRaisesRegex(ValueError, "Optimization changed"):
            validate(report, manifest, raw)


if __name__ == "__main__":
    unittest.main()
