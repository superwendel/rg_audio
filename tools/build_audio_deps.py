"""Build pinned tool-only audio libraries into build/deps/install (CMake required)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = [
    ("soxr", "0.1.3", "https://codeload.github.com/chirlu/soxr/tar.gz/refs/tags/0.1.3",
     "db6ca1b1e8405c6ef92f8294fc123d910abf0a114003b3f0f13fa57a95fd62d0",
     ["-DWITH_OPENMP=OFF", "-DWITH_LSR_BINDINGS=OFF", "-DBUILD_TESTS=OFF", "-DBUILD_EXAMPLES=OFF"]),
    ("libsndfile", "1.2.2", "https://codeload.github.com/libsndfile/libsndfile/tar.gz/refs/tags/1.2.2",
     "ffe12ef8add3eaca876f04087734e6e8e029350082f3251f565fa9da55b52121",
     ["-DENABLE_EXTERNAL_LIBS=OFF", "-DENABLE_MPEG=OFF", "-DBUILD_PROGRAMS=OFF",
      "-DBUILD_EXAMPLES=OFF", "-DBUILD_TESTING=OFF"]),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    cache = ROOT / "build/deps"
    cache.mkdir(parents=True, exist_ok=True)
    prefix = cache / "install"
    manifest = []
    for name, version, url, digest, options in PACKAGES:
        archive = cache / (name + ".tar.gz")
        if not archive.exists():
            urllib.request.urlretrieve(url, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Source archive hash mismatch: {archive}")
        source = cache / f"{name}-{version}"
        if not source.exists():
            with tarfile.open(archive) as bundle:
                bundle.extractall(cache, filter="data")
        build = cache / (name + "-cmake")
        # Allow CMake to select the native platform generator, including VS on Windows.
        subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
                        "-DBUILD_SHARED_LIBS=ON", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", *options], check=True)
        subprocess.run(["cmake", "--build", str(build), "--config", "Release",
                        "--parallel", str(args.jobs)], check=True)
        subprocess.run(["cmake", "--install", str(build), "--config", "Release"], check=True)
        manifest.append({"name": name, "version": version, "url": url, "sha256": digest})
    (prefix / "sources.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Audio tool dependencies installed to {prefix}")


if __name__ == "__main__":
    main()
