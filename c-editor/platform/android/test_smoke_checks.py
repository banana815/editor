import importlib.util
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location('smoke_checks', Path(__file__).with_name('smoke_checks.py'))
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)


def activities(sequence):
    return (f'  mGlobalConfiguration: {{1.0 en-US sw320dp as.{sequence} s.42}}\n'
            '  ResumedActivity: ActivityRecord{abcd u0 example.launcher/.Home t1}\n')


class SmokeChecks(unittest.TestCase):
    def test_readiness_requires_boot_unlock_home_and_config(self):
        good = ['1', 'stopped', 'RUNNING_UNLOCKED', activities(8), 'example.launcher/.Home']
        self.assertTrue(checks.readiness_snapshot(*good)['ready'])
        for index, bad in [(0, '0'), (1, 'running'), (2, 'RUNNING_LOCKED'),
                           (3, 'no configuration'), (4, 'com.android.settings/.FallbackHome')]:
            values = good.copy(); values[index] = bad
            self.assertFalse(checks.readiness_snapshot(*values)['ready'])

    def test_asset_sequence_change_restarts_stability_window(self):
        elapsed = [0]
        def adb(*args, **kwargs):
            command = args[1:]
            if command == ('getprop', 'sys.boot_completed'): out = '1'
            elif command == ('getprop', 'init.svc.bootanim'): out = 'stopped'
            elif command == ('am', 'get-started-user-state', '0'): out = 'RUNNING_UNLOCKED'
            elif command[:3] == ('cmd', 'package', 'resolve-activity'): out = 'example.launcher/.Home'
            elif command == ('dumpsys', 'activity'): out = activities(8 if elapsed[0] < 8 else 9)
            else: self.fail(command)
            self.assertLessEqual(kwargs['timeout'], 15)
            return SimpleNamespace(stdout=out, returncode=0)
        with tempfile.TemporaryDirectory() as directory:
            checks.wait_for_ready(adb, Path(directory), clock=lambda: elapsed[0],
                                  sleep=lambda seconds: elapsed.__setitem__(0, elapsed[0] + seconds))
            self.assertEqual(elapsed[0], 18)
            self.assertIn('as.9', (Path(directory) / 'readiness.jsonl').read_text())

    def test_readiness_deadline_is_finite(self):
        elapsed = [0]
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(TimeoutError):
                checks.wait_for_ready(lambda *a, **kw: SimpleNamespace(stdout='', returncode=1), Path(directory),
                    timeout=3, clock=lambda: elapsed[0],
                    sleep=lambda seconds: elapsed.__setitem__(0, elapsed[0] + seconds))
        self.assertEqual(elapsed[0], 3)

    def test_only_complete_bmp_is_evidence(self):
        data = bytearray(54 + 100 * 100 * 4)
        data[:2] = b'BM'
        struct.pack_into('<I', data, 10, 54)
        struct.pack_into('<iiHH', data, 18, 100, -100, 1, 32)
        self.assertEqual(checks.bmp_dimensions(data), (100, 100))
        for bad in [b'cat: files/ci-smoke.bmp: No such file or directory\r\n', data[:53], data[:-1]]:
            with self.assertRaises(ValueError): checks.bmp_dimensions(bad)


if __name__ == '__main__':
    unittest.main()
