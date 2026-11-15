"""Readiness and screenshot checks shared by the Android CI runner and tests."""
import json
import re
import struct
import time


def bmp_dimensions(data):
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('Application did not produce a BMP screenshot')
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height, planes, bits = struct.unpack_from('<iiHH', data, 18)
    if width < 100 or abs(height) < 100 or planes != 1 or bits not in (24, 32):
        raise ValueError(f'Unexpected render dimensions: {width} x {height}')
    stride = ((width * bits + 31) // 32) * 4
    if offset < 54 or len(data) < offset + stride * abs(height):
        raise ValueError('Application screenshot pixels are incomplete')
    return width, abs(height)


def readiness_snapshot(boot, animation, user, activities, home):
    configuration = re.search(r'^\s*mGlobalConfiguration[:=]\s*(.+)$', activities, re.M)
    components = [line.strip() for line in home.splitlines()
                  if re.fullmatch(r'[\w.$]+/[\w.$]+', line.strip())]
    component = components[-1] if components else ''
    home_package = component.split('/')[0]
    resumed = [line.strip() for line in activities.splitlines()
               if re.search(r'(?:topResumedActivity|mResumedActivity|ResumedActivity)[:=]', line)]
    home_ready = bool(component and 'FallbackHome' not in component and
                      any(home_package + '/' in line for line in resumed))
    return {
        'boot': boot.strip(), 'animation': animation.strip(), 'user': user.strip(),
        'configuration': configuration.group(1) if configuration else '',
        'home': component, 'home_resumed': home_ready,
        'ready': (boot.strip() == '1' and animation.strip() == 'stopped' and
                  user.strip() == 'RUNNING_UNLOCKED' and bool(configuration) and home_ready),
    }


def wait_for_ready(adb, output, *, timeout=180, stable_seconds=10,
                   clock=time.monotonic, sleep=time.sleep):
    """Wait for this emulator; never relaunch the app or restart any services.

    Unlike `am get-config` resource qualifiers, mGlobalConfiguration includes
    the assets sequence (`as.N`). First-boot theme overlays can change only that
    field and force an Activity restart before SDL has started the C thread.
    """
    deadline = clock() + timeout
    previous = None
    stable_since = None
    with (output / 'readiness.jsonl').open('w', encoding='utf-8') as evidence:
        while clock() < deadline:
            def query(*arguments):
                remaining = deadline - clock()
                if remaining <= 0:
                    raise TimeoutError('Emulator readiness deadline expired')
                result = adb('shell', *arguments, check=False, timeout=min(15, remaining))
                return result.stdout if result.returncode == 0 else ''

            boot = query('getprop', 'sys.boot_completed')
            animation = query('getprop', 'init.svc.bootanim')
            user = query('am', 'get-started-user-state', '0')
            home = query('cmd', 'package', 'resolve-activity', '--brief', '--user', '0',
                         '-a', 'android.intent.action.MAIN', '-c', 'android.intent.category.HOME')
            activities = query('dumpsys', 'activity')
            snapshot = readiness_snapshot(boot, animation, user, activities, home)
            now = clock()
            fingerprint = (snapshot['configuration'], snapshot['home'])
            if not snapshot['ready']:
                stable_since = None
            elif fingerprint != previous or stable_since is None:
                stable_since = now
            previous = fingerprint
            snapshot['stable_seconds'] = 0 if stable_since is None else now - stable_since
            evidence.write(json.dumps(snapshot, ensure_ascii=False) + '\n')
            evidence.flush()
            if snapshot['ready'] and snapshot['stable_seconds'] >= stable_seconds:
                return
            sleep(min(1, max(0, deadline - clock())))
    raise TimeoutError('Emulator boot, user unlock, Home, and configuration did not stabilize; see readiness.jsonl')
