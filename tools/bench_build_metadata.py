"""Record the exact local native comparison build, including dependency binaries."""
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    dependency_root = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/deps/install"
    build = ROOT / "build/bench"
    build.mkdir(parents=True, exist_ok=True)
    compiler = subprocess.run(["cl"], capture_output=True, text=True)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    core = Path(os.environ.get("RG_CORE_DIR", str(ROOT.parent / "rg_core")))
    paths = [ROOT / "src/rg_rgs.h", ROOT / "build/baseline/src/rg_rgs.h",
             ROOT / "benchmarks/codec_adapter.c", ROOT / "tools/rgs_audio_prepare.h",
             ROOT / "third_party/qoa/qoa.h", core / "src/rg_time.h",
             build / "codec_candidate.exe", build / "codec_baseline.exe"]
    paths += list((dependency_root / "bin").glob("*.dll"))
    metadata = {"compiler": (compiler.stdout + compiler.stderr).strip(),
                "flags": "/std:c11 /W4 /WX /O2 (baseline additionally selects frozen header)",
                "link": "soxr.lib sndfile.lib; shared libraries", "os": platform.platform(),
                "candidate_base_revision": revision, "baseline_revision": "af980e8",
                "sha256": {str(path.resolve()): digest(path) for path in paths},
                "tool_dependencies": json.loads((dependency_root / "sources.json").read_text())
                    if (dependency_root / "sources.json").exists() else "external installation"}
    (build / "build_metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
