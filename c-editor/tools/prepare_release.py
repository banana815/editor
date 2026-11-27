"""Collect only tested distribution artifacts; reject incomplete releases."""

import argparse
import hashlib
import re
import shutil
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def prepare(artifacts, output, tag, cmake_file):
    version_match = re.search(r"project\(NoteLab VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b",
                              cmake_file.read_text(encoding="utf-8"))
    if not version_match or tag != "v" + version_match.group(1):
        raise ValueError("Release tag must exactly match the CMake project version (vX.Y.Z)")
    version = version_match.group(1)
    if output.exists() and any(output.iterdir()):
        raise ValueError("Release output must be empty")

    # Inspect every input before writing output. Never publish startup evidence,
    # test data, or a partial platform set as if it were a distribution.
    files = []
    for artifact, system, extension in (("Linux", "Linux", "tar.gz"),
                                         ("macOS", "Darwin", "zip"),
                                         ("Windows", "Windows", "zip")):
        matches = list(artifacts.glob(f"notelab-{artifact}-*/NoteLab-{version}-{system}-*.{extension}"))
        if len(matches) != 1:
            raise ValueError(f"Expected one {system} package, found {len(matches)}")
        files.append((matches[0], matches[0].name))
    files.append((artifacts / "notelab-android-unsigned/app-release-unsigned.apk",
                  f"NoteLab-{version}-Android-unsigned.apk"))
    for target in ("device", "simulator"):
        files.append((artifacts / f"notelab-ios-{target}-unsigned/notelab-ios-{target}-unsigned.tar.gz",
                      f"NoteLab-{version}-iOS-{target}-unsigned.tar.gz"))

    browser = artifacts / "notelab-browser"
    browser_files = [browser / name for name in
                     ("notelab.html", "notelab.js", "notelab.wasm", "notelab.data", "THIRD_PARTY.md")]
    licenses = [path for path in (browser / "licenses").rglob("*") if path.is_file()]
    if not licenses:
        raise ValueError("Browser licenses are missing")
    browser_files += licenses
    for path in [source for source, _ in files] + browser_files:
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Missing or empty release input: {path}")

    output.mkdir(parents=True, exist_ok=True)
    for source, name in files:
        shutil.copyfile(source, output / name)
    with ZipFile(output / f"NoteLab-{version}-Web.zip", "w", ZIP_DEFLATED) as archive:
        for path in sorted(browser_files):
            archive.write(path, path.relative_to(browser).as_posix())
    checksums = []
    for path in sorted(output.iterdir()):
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        checksums.append(f"{digest.hexdigest()}  {path.name}\n")
    (output / "SHA256SUMS.txt").write_text("".join(checksums), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--cmake", type=Path,
                        default=Path(__file__).resolve().parents[1] / "CMakeLists.txt")
    args = parser.parse_args()
    prepare(args.artifacts, args.output, args.tag, args.cmake)
