"""Fetch the original QOA comparison WAVs; keep audio in ignored build storage."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path, PurePosixPath
import urllib.request
from urllib.parse import quote
import wave

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "https://qoaformat.org/samples/"
MANIFEST = Path(__file__).with_name("corpus_manifest.json")


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.paths = set()

    def handle_starttag(self, tag, attrs):
        for key, value in attrs:
            if key not in ("src", "href") or not value:
                continue
            path = PurePosixPath(value)
            if ("\\" not in value and value.endswith(".wav") and not value.endswith(".qoa.wav")
                    and not path.is_absolute() and ".." not in path.parts
                    and len(path.parts) == 2
                    and path.parts[0] in ("bandcamp", "oculus_audio_pack", "sqam")):
                self.paths.add(value)


def retrieve(entry, destination):
    destination = destination.resolve()
    path = (destination / entry["relative_path"]).resolve()
    if not path.is_relative_to(destination):
        raise ValueError("Corpus destination escapes the selected cache")
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        temporary = path.with_suffix(".wav.part")
        with urllib.request.urlopen(entry["original_url"], timeout=90) as source:
            with temporary.open("wb") as output:
                while chunk := source.read(1024 * 1024):
                    output.write(chunk)
        temporary.replace(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if entry.get("sha256") and digest != entry["sha256"]:
        raise ValueError(f"Corpus hash mismatch: {path}")
    with wave.open(str(path), "rb") as audio:
        metadata = {"frames": audio.getnframes(), "channels": audio.getnchannels(),
                    "samplerate": audio.getframerate(), "sample_width": audio.getsampwidth()}
        if metadata["sample_width"] != 2 or audio.getcomptype() != "NONE":
            raise ValueError(f"Expected original PCM16 WAV: {path}")
    return dict(entry, sha256=digest, bytes=path.stat().st_size, **metadata)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=ROOT / "build/corpus/original")
    parser.add_argument("--refresh-manifest", action="store_true")
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    if MANIFEST.exists() and not args.refresh_manifest:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        entries = document["samples"]
    else:
        with urllib.request.urlopen(SOURCE, timeout=30) as response:
            html = response.read().decode("utf-8")
        links = Links()
        links.feed(html)
        if not links.paths:
            raise ValueError("No original WAV links found; refusing an empty corpus")
        entries = [{"relative_path": path, "original_url": SOURCE + quote(path),
                    "category": path.split("/")[0]} for path in sorted(links.paths)]
        document = {"source": SOURCE, "page_sha256": hashlib.sha256(html.encode()).hexdigest(),
                    "provenance": {
                        "bandcamp": "Artist excerpts hosted by the QOA sample site; no blanket redistribution license asserted.",
                        "oculus_audio_pack": "Oculus Audio Pack 1 excerpts hosted by the QOA sample site.",
                        "sqam": "EBU Sound Quality Assessment Material hosted by the QOA sample site."}}
    completed = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        pending = [pool.submit(retrieve, entry, args.destination) for entry in entries]
        for future in as_completed(pending):
            completed.append(future.result())
            if len(completed) % 10 == 0 or len(completed) == len(entries):
                print(f"Verified {len(completed)}/{len(entries)} originals", flush=True)
    document["samples"] = sorted(completed, key=lambda entry: entry["relative_path"])
    if args.refresh_manifest or not MANIFEST.exists():
        MANIFEST.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"{len(completed)} samples, {sum(x['bytes'] for x in completed):,} bytes; {args.destination}")


if __name__ == "__main__":
    main()
