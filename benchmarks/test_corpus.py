"""Corpus downloader must never turn remote paths into writes outside its cache."""
from pathlib import Path
import tempfile
import unittest

from corpus import Links, retrieve


class CorpusPaths(unittest.TestCase):
    def test_only_original_relative_wavs(self):
        links = Links()
        links.feed('''<audio src="sqam/original.wav"></audio>
          <audio src="sqam/qoa_wav/original.qoa.wav"></audio>
          <audio src="../escape.wav"></audio>
          <audio src="/sqam/absolute.wav"></audio>
          <audio src="oculus_audio_pack/..\\..\\escape.wav"></audio>''')
        self.assertEqual(links.paths, {"sqam/original.wav"})

    def test_manifest_escape_rejected_before_network(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "cache"
            with self.assertRaisesRegex(ValueError, "escapes"):
                retrieve({"relative_path": "../escape.wav", "original_url": "unused"}, cache)
            self.assertFalse(cache.exists())


if __name__ == "__main__":
    unittest.main()
