#!/usr/bin/env python3
"""Install a debug APK on an explicitly selected emulator and verify C startup."""
import argparse
from pathlib import Path
import subprocess
import sys
import time
from smoke_checks import bmp_dimensions, wait_for_ready

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
# SDL3 native logs use category tags; plain SDL only contains Java lifecycle.
log_filters = ('SDL:V', *(f'SDL/{category}:V' for category in
               ('APP', 'ERROR', 'ASSERT', 'SYSTEM', 'AUDIO', 'VIDEO', 'RENDER', 'INPUT', 'TEST', 'GPU')),
               'AndroidRuntime:E', 'libc:F', 'DEBUG:V', 'tombstoned:V')


def adb(*arguments, check=True, binary=False, timeout=120):
    result = subprocess.run(['adb', '-s', args.serial, *arguments],
                            capture_output=True, text=not binary, timeout=timeout)
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
    (args.output / 'android-smoke.bmp').unlink(missing_ok=True)
    adb('shell', 'run-as', package, 'rm', '-f', 'files/ci-smoke.bmp')
    wait_for_ready(adb, args.output)
    adb('logcat', '-c')
    launched = adb('shell', 'am', 'start', '-W', '-n', package + '/.NoteLabActivity',
                   '--ez', 'notelabSmokeTest', 'true')
    (args.output / 'launch.log').write_text(launched.stdout + launched.stderr)
    deadline = time.monotonic() + 90
    log = ''
    while time.monotonic() < deadline:
        log = adb('logcat', '-d', '-s', *log_filters).stdout
        if 'NoteLab smoke test passed' in log:
            break
        if ('Finished main function' in log or 'FATAL EXCEPTION' in log or
                'activity finished' in log):
            raise RuntimeError('C application exited without the smoke success marker; see logcat.log')
        time.sleep(0.5)
    else:
        raise RuntimeError('C application did not report a successful startup within 90 seconds')
    shot = adb('exec-out', 'run-as', package, 'cat', 'files/ci-smoke.bmp', binary=True).stdout
    width, height = bmp_dimensions(shot)
    (args.output / 'android-smoke.bmp').write_bytes(shot)
    print(f'Android C startup and rendering passed ({width} x {abs(height)})')
finally:
    # Keep logs even if app startup or screenshot validation failed.
    logs = adb('logcat', '-d', '-s', *log_filters, check=False)
    (args.output / 'logcat.log').write_text(logs.stdout + logs.stderr)
    all_logs = adb('logcat', '-d', '-b', 'all', check=False)
    (args.output / 'logcat-all.log').write_text(all_logs.stdout + all_logs.stderr)
    if not (args.output / 'android-smoke.bmp').exists():
        # Preserve diagnostic pixels even when the success marker was absent.
        shot = adb('exec-out', 'run-as', package, 'cat', 'files/ci-smoke.bmp', check=False, binary=True)
        try:
            bmp_dimensions(shot.stdout)
        except ValueError:
            # `adb exec-out` can return zero even when remote cat emits an
            # error. Keep diagnostics as text, never masquerading as a BMP.
            (args.output / 'screenshot-error.log').write_text(
                (shot.stdout + shot.stderr).decode('utf-8', errors='replace'))
        else:
            if shot.returncode == 0:
                (args.output / 'android-smoke.bmp').write_bytes(shot.stdout)
    adb('uninstall', package, check=False)
