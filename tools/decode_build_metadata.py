"""Record the native decode-only comparison build and its selected baseline."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = ROOT / "benchmarks/baselines/rg_rgs_2026_09_26.h"
DEFAULT_BASELINE_SHA256 = "90ca4ac05ed3aa3eba8a72ce6b48a9177333486dce63163d895f7cb67fd9d9ac"


def file_record(path):
    return {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-baseline", action="store_true", help="validate the selected source before compilation")
    args = parser.parse_args()
    core = Path(os.environ.get("RG_CORE_DIR", str(ROOT.parent / "rg_core")))
    build = Path(os.environ.get("RG_AUDIO_DECODE_BUILD_DIR", str(ROOT / "build/decode-profile"))).resolve()
    baseline_path = Path(os.environ.get("RG_AUDIO_DECODE_BASELINE", str(DEFAULT_BASELINE))).resolve()
    baseline = file_record(baseline_path)
    expected = os.environ.get("RG_AUDIO_DECODE_BASELINE_SHA256")
    if expected is None and baseline_path == DEFAULT_BASELINE.resolve():
        expected = DEFAULT_BASELINE_SHA256
    if expected is not None and baseline["sha256"] != expected.lower():
        raise ValueError("The decoder baseline differs from the expected SHA-256")
    if args.check_baseline:
        return
    flags = os.environ.get("RUNTIME_FLAGS", "/nologo /std:c11 /W4 /WX /O2")
    baseline_define = r'"/DRG_RGS_HEADER=\"' + baseline_path.as_posix() + r'\""'
    compiler = subprocess.run(["cl"], capture_output=True, text=True, errors="replace", check=False)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    inputs = {
        "harness": ROOT / "benchmarks/decode_profile.c",
        "candidate_header": ROOT / "src/rg_rgs.h",
        "core_defs": core / "src/rg_defs.h",
        "core_time": core / "src/rg_time.h",
        "qoa_header": ROOT / "third_party/qoa/qoa.h",
        "build_script": ROOT / "build.bat",
        "metadata_script": Path(__file__),
    }
    metadata = {
        "schema_version": 1,
        "compiler": (compiler.stdout + compiler.stderr).strip(),
        "compiler_path": shutil.which("cl"),
        "flags": flags,
        "baseline_additional_flags": baseline_define,
        "working_directory": str(ROOT),
        "core_directory": str(core.resolve()),
        "os": platform.platform(),
        "candidate_base_revision": revision,
        "baseline_source": baseline,
        "baseline_expected_sha256": expected,
        "external_codec_libraries": [],
        "input_hashes": {name: file_record(path) for name, path in inputs.items()},
        "executables": {variant: file_record(build / (variant + ".exe"))
                        for variant in ("baseline", "candidate")},
        "commands": {
            variant: f"cl {flags} " + (baseline_define + " " if variant == "baseline" else "") +
                     f'benchmarks\\decode_profile.c /Fo:"{build / (variant + ".obj")}" ' +
                     f'/Fe:"{build / (variant + ".exe")}"'
            for variant in ("baseline", "candidate")
        },
    }
    (build / "build_metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
