#!/usr/bin/env python3
"""Extract the desktop deliverable and launch it away from the build tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import stat
import struct
import subprocess
import tarfile
import tempfile
import zipfile


def extract(archive: Path, destination: Path) -> None:
    def check(name: str) -> None:
        path = Path(name)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError(f"Unsafe archive member: {name}")

    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as source:
            for member in source.infolist():
                check(member.filename)
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise ValueError("Unexpected symlink in desktop archive")
                output = Path(source.extract(member, destination))
                if mode & 0o111 and output.is_file():
                    output.chmod(output.stat().st_mode | (mode & 0o111))
    else:
        with tarfile.open(archive, "r:gz") as source:
            for member in source.getmembers():
                check(member.name)
                if not (member.isfile() or member.isdir()):
                    raise ValueError("Unexpected special file in desktop archive")
            source.extractall(destination, filter="data")


def windows_imports(binary: Path) -> list[str]:
    """Read actual PE imports, without relying on tools or DLLs on the runner."""
    data = binary.read_bytes()
    assert data[:2] == b"MZ"
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    assert magic in (0x10B, 0x20B)
    directory = optional + (112 if magic == 0x20B else 96)
    assert directory + 16 <= optional + optional_size
    import_rva, import_size = struct.unpack_from("<II", data, directory + 8)
    assert import_rva and import_size >= 20, "Expected Windows system imports"
    sections = optional + optional_size

    def offset(rva: int) -> int:
        for index in range(section_count):
            section = sections + index * 40
            virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", data, section + 8)
            if address <= rva < address + max(virtual_size, raw_size):
                delta = rva - address
                assert delta < raw_size and raw + delta < len(data)
                return raw + delta
        raise AssertionError(f"PE RVA is not in a file-backed section: {rva:#x}")

    imported = []
    for index in range(import_size // 20):
        descriptor = offset(import_rva + index * 20)
        values = struct.unpack_from("<IIIII", data, descriptor)
        if not any(values):
            return imported
        name = offset(values[3])
        end = data.find(b"\0", name, min(name + 4096, len(data)))
        assert end > name
        imported.append(data[name:end].decode("ascii").lower())
    raise AssertionError("PE import table is not terminated")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", type=Path)
    args = parser.parse_args()
    archives = sorted(args.packages.glob("NoteLab-*.zip")) + sorted(args.packages.glob("NoteLab-*.tar.gz"))
    if len(archives) != 1:
        raise AssertionError(f"Expected exactly one platform archive, found {archives}")
    with tempfile.TemporaryDirectory(prefix="notelab-distribution-") as temporary:
        work = Path(temporary)
        unpacked = work / "unpacked"
        unpacked.mkdir()
        extract(archives[0], unpacked)
        roots = list(unpacked.iterdir())
        assert len(roots) == 1 and roots[0].is_dir(), roots
        root = roots[0]
        if platform.system() == "Darwin":
            app = root / "notelab.app"
            binary = app / "Contents/MacOS/notelab"
            assets = app / "Contents/Resources/assets"
            with (app / "Contents/Info.plist").open("rb") as source:
                info = plistlib.load(source)
            assert info["CFBundleIdentifier"] == "org.notelab.editor"
            dependencies = subprocess.check_output(["otool", "-L", binary], text=True)
            for line in dependencies.splitlines()[1:]:
                library = line.strip().split(" (", 1)[0]
                assert library.startswith(("/System/Library/", "/usr/lib/")), library
        else:
            binary = root / ("notelab.exe" if os.name == "nt" else "notelab")
            assets = root / "assets"
            if os.name == "nt":
                imports = windows_imports(binary)
                assert imports
                external_runtime = re.compile(r"^(?:vcruntime|msvcp|msvcr|ucrtbase|api-ms-win-crt-)", re.I)
                assert not any(external_runtime.match(name) for name in imports), imports
            if platform.system() == "Linux":
                dependencies = subprocess.check_output(["ldd", binary], text=True)
                assert "not found" not in dependencies, dependencies
                assert not re.search(r"lib(?:SDL|cjson|harfbuzz|freetype|sqlite)", dependencies, re.I), dependencies
        assert binary.is_file()
        for font in ("font.otf", "emoji.ttf"):
            assert (assets / font).stat().st_size > 1_000_000, font
        for license_name in ("OFL.txt", "OFL-Emoji.txt"):
            assert (assets / license_name).stat().st_size > 100, license_name
        assert (root / "THIRD_PARTY.md").stat().st_size > 100
        assert (root / "README.md").is_file()
        license_directories = [root / "licenses"]
        if platform.system() == "Darwin":
            assert (assets / "THIRD_PARTY.md").is_file()
            license_directories.append(assets / "licenses")
        for directory in license_directories:
            manifest = json.loads((directory / "SOURCES.json").read_text(encoding="utf-8"))
            assert manifest["licenses"]
            for license_entry in manifest["licenses"]:
                contents = (directory / license_entry["file"]).read_bytes()
                assert hashlib.sha256(contents).hexdigest() == license_entry["sha256"], license_entry["file"]

        notes = work / "独立启动笔记"
        screenshot = work / "screen.bmp"
        environment = os.environ.copy()
        for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"):
            environment.pop(key, None)
        environment["SDL_VIDEODRIVER"] = "dummy"
        result = subprocess.run(
            [str(binary), "--smoke-test", "--notes", str(notes), "--screenshot", str(screenshot)],
            cwd=work, env=environment, capture_output=True, text=True, timeout=45,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert "NoteLab smoke test passed" in result.stdout + result.stderr
        with screenshot.open("rb") as source:
            header = source.read(54)
        assert header[:2] == b"BM"
        width, height = struct.unpack_from("<ii", header, 18)
        assert width >= 320 and abs(height) >= 240
        assert screenshot.stat().st_size >= 54 + width * abs(height) * 3
        print(f"PASS: {archives[0].name}; extracted app launched with bundled fonts ({width}×{abs(height)})")


if __name__ == "__main__":
    main()
