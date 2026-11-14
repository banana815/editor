#!/usr/bin/env python3
"""Install a debug APK on an explicitly selected emulator and verify C startup."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--serial', required=True)
parser.add_argument('--apk', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
if not args.serial.startswith('emulator-') or not args.serial[9:].isdigit():
    parser.error('Use an isolated emulator serial, not a physical device')
if not args.apk.is_file():
    parser.error('Debug APK does not exist')
args.output.mkdir(parents=True, exist_ok=True)
package = 'org.notelab.editor'


def adb(*arguments, check=True, binary=False):
    result = subprocess.run(['adb', '-s', args.serial, *arguments],
                            capture_output=True, text=not binary, timeout=120)
    if check and result.returncode:
        print(result.stderr.decode(errors='replace') if binary else result.stderr, file=sys.stderr)
        if not binary:
            print(result.stdout, file=sys.stderr)
        result.check_returncode()
    return result


# The explicit emulator is owned by the CI runner; no connected devices or
# existing user installations are discovered or modified by this script.
adb('wait-for-device')
adb('install', '-r', str(args.apk.resolve()))
try:
    adb('shell', 'run-as', package, 'rm', '-f', 'files/ci-smoke.bmp')
    adb('logcat', '-c')
    launched = adb('shell', 'am', 'start', '-W', '-n', package + '/.NoteLabActivity',
                   '--ez', 'notelabSmokeTest', 'true')
    (args.output / 'launch.log').write_text(launched.stdout + launched.stderr)
    deadline = time.monotonic() + 90
    log = ''
    while time.monotonic() < deadline:
        log = adb('logcat', '-d', '-s', 'SDL:V', 'AndroidRuntime:E', 'libc:F').stdout
        if 'NoteLab smoke test passed' in log:
            break
        time.sleep(0.5)
    else:
        raise RuntimeError('C application did not report a successful startup within 90 seconds')
    shot = adb('exec-out', 'run-as', package, 'cat', 'files/ci-smoke.bmp', binary=True).stdout
    if len(shot) < 54 or shot[:2] != b'BM':
        raise RuntimeError('Application did not produce a BMP screenshot')
    width, height = struct.unpack_from('<ii', shot, 18)
    if width < 100 or abs(height) < 100:
        raise RuntimeError(f'Unexpected render dimensions: {width} x {height}')
    (args.output / 'android-smoke.bmp').write_bytes(shot)
    print(f'Android C startup and rendering passed ({width} x {abs(height)})')
finally:
    # Keep logs even if app startup or screenshot validation failed.
    logs = adb('logcat', '-d', '-s', 'SDL:V', 'AndroidRuntime:E', 'libc:F', check=False)
    (args.output / 'logcat.log').write_text(logs.stdout + logs.stderr)
    adb('uninstall', package, check=False)
