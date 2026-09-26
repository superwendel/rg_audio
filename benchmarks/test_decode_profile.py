"""Decode-profiler correctness checks using generated, multi-frame PCM fixtures.

Build the optional adapters with build.bat bench_compare and bench_decode first.
No downloaded corpus is needed. Tests skip when those executables are absent.
Do not run this suite alongside performance measurements.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave

from run_decode_profile import ROOT, invoke


ADAPTER = ROOT / "build/bench/codec_candidate.exe"
DECODE_BUILD_DIR = (ROOT / os.environ.get("RG_AUDIO_DECODE_BUILD_DIR", "build/decode-profile")).resolve()
CANDIDATE = DECODE_BUILD_DIR / "candidate.exe"
BASELINE = DECODE_BUILD_DIR / "baseline.exe"


def encode_fixture(source, encoded, codec, quality="medium"):
    environment = os.environ.copy()
    environment["PATH"] = str(ROOT / "build/deps/install/bin") + os.pathsep + environment.get("PATH", "")
    completed = subprocess.run(
        [str(ADAPTER), "--input", str(source), "--encoded", str(encoded),
         "--codec", codec, "--quality", quality, "--min-ms", "0"],
        capture_output=True, text=True, env=environment)
    if completed.returncode:
        raise RuntimeError(f"Fixture encoding failed: {completed.stderr.strip() or completed.stdout.strip()}")
    return json.loads(completed.stdout)


@unittest.skipUnless(ADAPTER.exists() and CANDIDATE.exists(), "optional decode-profiler executables not built")
class DecodeProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="rgs_decode_profile_test_")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.frames = 2 * 5120 + 137  # Three frames, including a partial final slice.
        cls.channels = 2
        cls.wav = cls.directory / "source.wav"
        pcm = bytearray()
        for frame in range(cls.frames):
            for channel in range(cls.channels):
                sample = ((frame * 97 + channel * 131) % 65536) - 32768
                pcm.extend(struct.pack("<h", sample))
        with wave.open(str(cls.wav), "wb") as file:
            file.setnchannels(cls.channels)
            file.setsampwidth(2)
            file.setframerate(44100)
            file.writeframes(pcm)
        cls.inputs = {}
        for quality in ("high", "medium", "low"):
            path = cls.directory / f"{quality}.rgs"
            cls.inputs[quality] = (path, encode_fixture(cls.wav, path, "rgs", quality))
        cls.qoa = cls.directory / "source.qoa"
        cls.qoa_reference = encode_fixture(cls.wav, cls.qoa, "qoa")
        cls.executables = [CANDIDATE] + ([BASELINE] if BASELINE.exists() else [])

    def test_checked_trusted_match_frozen_decoder_and_each_other(self):
        for executable in self.executables:
            for quality, (path, reference) in self.inputs.items():
                with self.subTest(executable=executable.name, quality=quality):
                    checked = invoke(executable, path, "checked", 0)
                    trusted = invoke(executable, path, "trusted", 0)
                    self.assertEqual(checked["decoded_fnv64"], reference["decoded_fnv64"])
                    self.assertEqual(trusted["decoded_fnv64"], checked["decoded_fnv64"])
                    self.assertEqual(trusted["encoded_fnv64"], reference["encoded_fnv64"])
                    self.assertTrue(trusted["checked_decode_preflight"])
                    self.assertFalse(checked["checked_decode_preflight"])
                    self.assertEqual(checked["frames"], self.frames)
                    self.assertEqual(checked["channels"], self.channels)
                    self.assertEqual(checked["rate"], 44100)
                    self.assertEqual(checked["codec_frames"], 3)
                    self.assertEqual(checked["output_buffer_bytes"], self.frames * self.channels * 2)

    def test_parse_validates_all_frames_without_pcm(self):
        for executable in self.executables:
            for quality, (path, reference) in self.inputs.items():
                with self.subTest(executable=executable.name, quality=quality):
                    parsed = invoke(executable, path, "parse", 0)
                    self.assertEqual(parsed["frames"], self.frames)
                    self.assertEqual(parsed["codec_frames"], 3)
                    self.assertEqual(parsed["encoded_bytes"], path.stat().st_size)
                    self.assertEqual(parsed["encoded_fnv64"], reference["encoded_fnv64"])
                    self.assertEqual(parsed["output_buffer_bytes"], 0)
                    self.assertIsNone(parsed["decoded_fnv64"])
                    self.assertFalse(parsed["checked_decode_preflight"])

    def test_qoa_matches_reference_fingerprint(self):
        for executable in self.executables:
            with self.subTest(executable=executable.name):
                decoded = invoke(executable, self.qoa, "qoa", 0)
                self.assertEqual(decoded["decoded_fnv64"], self.qoa_reference["decoded_fnv64"])
                self.assertEqual(decoded["encoded_fnv64"], self.qoa_reference["encoded_fnv64"])
                self.assertEqual(decoded["frames"], self.frames)
                self.assertEqual(decoded["codec_frames"], 3)

    def test_all_modes_reject_truncated_or_trailing_stream_bytes(self):
        for codec, path, modes in (("rgs", self.inputs["medium"][0], ("checked", "trusted", "parse")),
                                  ("qoa", self.qoa, ("qoa",))):
            original = path.read_bytes()
            for corruption, content in (("truncated", original[:-1]), ("trailing", original + b"\x00")):
                damaged = self.directory / f"{codec}_{corruption}.bin"
                damaged.write_bytes(content)
                for executable in self.executables:
                    for mode in modes:
                        with self.subTest(executable=executable.name, mode=mode, corruption=corruption), self.assertRaises(RuntimeError):
                            invoke(executable, damaged, mode, 0)

    def test_invalid_minimum_times_rejected_by_argument_parser(self):
        path = self.inputs["medium"][0]
        for value in ("nan", "inf", "-inf", "", "-1", "60001", "invalid"):
            with self.subTest(value=value):
                completed = subprocess.run(
                    [str(CANDIDATE), "--input", str(path), "--mode", "checked", "--min-ms", value],
                    capture_output=True, text=True)
                self.assertEqual(completed.returncode, 2)
                self.assertEqual(completed.stdout, "")


if __name__ == "__main__":
    unittest.main()
