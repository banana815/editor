#!/usr/bin/env python3
"""Fetch the exact SDL source used by CMake, including its Android Java bridge."""
import hashlib
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

editor = Path(__file__).resolve().parents[2]
declarations = (editor / 'cmake/Dependencies.cmake').read_text()
match = re.search(r'FetchContent_Declare\(editor_sdl\s+URL\s+(\S+)\s+URL_HASH SHA256=([0-9a-f]+)', declarations)
if not match:
    raise SystemExit('Cannot locate the pinned SDL archive in cmake/Dependencies.cmake')
url, digest = match.groups()
destination = editor / 'build-android-deps/SDL'
marker = destination / '.notelab-source-sha256'
if marker.is_file() and marker.read_text().strip() == digest:
    print(f'SDL already prepared: {destination}')
    raise SystemExit(0)
if destination.exists():
    raise SystemExit(f'{destination} already exists without the matching pin; choose a fresh build directory')
destination.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
    stage = Path(temporary)
    archive = stage / 'SDL.tar.gz'
    subprocess.run(['curl', '--fail', '--location', '--retry', '2', '--connect-timeout', '15',
                    '--max-time', '180', url, '-o', str(archive)], check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise SystemExit('SDL archive checksum mismatch')
    unpacked = stage / 'unpacked'
    unpacked.mkdir()
    with tarfile.open(archive) as source:
        # Python 3.12+ rejects traversal and escaping symlinks in downloaded archives.
        source.extractall(unpacked, filter='data')
    roots = list(unpacked.iterdir())
    if len(roots) != 1 or not (roots[0] / 'android-project').is_dir():
        raise SystemExit('Unexpected SDL archive layout')
    shutil.move(str(roots[0]), destination)
    marker.write_text(digest + '\n')
print(f'SDL prepared: {destination}')
