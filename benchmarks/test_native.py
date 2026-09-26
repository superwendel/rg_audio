"""Native harness regression tests; skipped when optional adapters are unbuilt.

Run after build.bat bench_compare and build.bat bench_workloads. Fixtures are
generated locally and require no downloaded corpus or audio device.
"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave

import numpy as np
from run_benchmarks import ROOT, native, read_pcm


CANDIDATE = ROOT / "build/bench/codec_candidate.exe"
STREAM = ROOT / "build/bench/stream_candidate.exe"


def mixed_checksum(samples):
    value = 14695981039346656037
    for sample in samples.reshape(-1):
        value = ((value ^ (int(sample) & 0xffffffff)) * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


@unittest.skipUnless(CANDIDATE.exists(), "optional native benchmark adapter not built")
class NativeHarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="rgs_benchmark_test_")
        cls.directory = Path(cls.temp.name)
        cls.wav = cls.directory / "input.wav"
        samples = ((np.arange(1003, dtype=np.int32) * 7919 % 60000) - 30000).astype("<i2")
        with wave.open(str(cls.wav), "wb") as file:
            file.setnchannels(1); file.setsampwidth(2); file.setframerate(44100)
            file.writeframes(samples.tobytes())
        cls.rgs = cls.directory / "input.rgs"
        cls.decoded = cls.directory / "decoded.wav"
        native(CANDIDATE, "--input", cls.wav, "--codec", "rgs", "--encoded", cls.rgs,
               "--decoded", cls.decoded, "--min-ms", 0)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_codecs_preserve_source_timeline_and_repeat_hashes(self):
        for codec in ("rgs", "qoa", "pcm", "ima"):
            with self.subTest(codec=codec):
                first = native(CANDIDATE, "--input", self.wav, "--codec", codec, "--min-ms", 0)
                second = native(CANDIDATE, "--input", self.wav, "--codec", codec, "--min-ms", 0)
                self.assertEqual(first["stored_frames"], 1003)
                self.assertEqual(first["stored_rate"], 44100)
                self.assertEqual(first["encoded_fnv64"], second["encoded_fnv64"])
                self.assertEqual(first["decoded_fnv64"], second["decoded_fnv64"])
                self.assertGreater(first["encoded_bytes"], 0)

    def test_invalid_timing_arguments_fail(self):
        for value in ("nan", "inf", "", "-1", "nonsense"):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                native(CANDIDATE, "--input", self.wav, "--min-ms", value)

    @unittest.skipUnless(STREAM.exists(), "optional native stream workload not built")
    def test_mix_loops_starts_and_eof_against_independent_pcm_reference(self):
        pcm, _ = read_pcm(self.decoded)
        for scenario, blocks in (("steady", 8), ("loop", 40), ("start", 40)):
            for voices in (1, 16):
                with self.subTest(scenario=scenario, voices=voices):
                    row = native(STREAM, "--input", self.rgs, "--voices", voices,
                                 "--callback-frames", 256, "--blocks", blocks, "--scenario", scenario)
                    count = row["blocks"] * 256
                    if scenario == "loop":
                        reference = np.tile(pcm, (count // len(pcm) + 1, 1))[:count]
                    else:
                        reference = np.zeros((count, 1))
                        starts = range(0, count, 32 * 256) if scenario == "start" else (0,)
                        for start in starts:
                            take = min(len(pcm), count - start)
                            reference[start:start + take] = pcm[:take]
                    self.assertEqual(row["checksum"], mixed_checksum(reference * voices))
                    self.assertEqual(row["steady_state_allocations"], 0)

    @unittest.skipUnless(STREAM.exists(), "optional native stream workload not built")
    def test_short_loop_rejected_before_workload(self):
        short_wav = self.directory / "short.wav"
        short_rgs = self.directory / "short.rgs"
        with wave.open(str(short_wav), "wb") as file:
            file.setnchannels(1); file.setsampwidth(2); file.setframerate(44100)
            file.writeframes(np.zeros(129, dtype="<i2").tobytes())
        native(CANDIDATE, "--input", short_wav, "--codec", "rgs", "--encoded", short_rgs, "--min-ms", 0)
        with self.assertRaisesRegex(RuntimeError, "at least one callback"):
            native(STREAM, "--input", short_rgs, "--callback-frames", 512, "--scenario", "loop", "--blocks", 4)


if __name__ == "__main__":
    unittest.main()
