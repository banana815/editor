"""Exercise the real shell runner with isolated, fake Apple command-line tools."""
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

UUID = '00000000-0000-4000-8000-000000000001'
MOCK = r'''
import json, os, pathlib, shutil, struct, sys
root = pathlib.Path(os.environ['NOTELAB_SMOKE_MOCK_ROOT'])
tool = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
uuid = '00000000-0000-4000-8000-000000000001'
mode = os.environ['NOTELAB_SMOKE_MOCK_MODE']
with (root / 'calls.jsonl').open('a') as log:
    log.write(json.dumps([tool, *args]) + '\n')
calls = [json.loads(line) for line in (root / 'calls.jsonl').read_text().splitlines()]
def count(*prefix):
    return sum(call[:len(prefix)] == list(prefix) for call in calls)
if tool == 'sleep':
    assert args in (['5'], ['10']), args
    sys.exit(0)  # Test retry ordering without real waits.
if tool == 'ditto':
    shutil.copytree(args[0], args[1]); sys.exit(0)
if tool == 'codesign': sys.exit(0)
assert tool == 'xcrun', tool
if args[0] == 'vtool': print('platform IOSSIMULATOR'); sys.exit(0)
assert args[0] == 'simctl', args
command = args[1]
if command == 'list':
    if 'devices' in args:
        if mode == 'list_timeout' or (mode == 'list_timeout_once' and count('xcrun', 'simctl', 'list', 'devices') == 1):
            sys.exit(124)
        if mode == 'malformed_state': print('{'); sys.exit(0)
        device = {'udid': uuid if mode != 'wrong_uuid' else '00000000-0000-4000-8000-000000000002',
                  'state': 'Shutdown' if mode == 'not_booted' else 'Booted'}
        print(json.dumps({'devices': {'runtime': [device] * (2 if mode == 'duplicate_uuid' else 1)}}))
    else:
        print(json.dumps({'runtimes': [{'isAvailable': True, 'identifier': 'com.apple.CoreSimulator.SimRuntime.iOS-99-1', 'version': '99.1', 'name': 'Mock iOS'}],
                          'devicetypes': [{'identifier': 'com.apple.CoreSimulator.SimDeviceType.iPhone-16', 'name': 'iPhone 16', 'productFamily': 'iPhone'}]}))
elif command == 'create': print(uuid)
elif command in ('boot', 'shutdown', 'delete', 'erase'):
    assert args[2] == uuid
    if command == 'boot' and mode == 'boot_error': sys.exit(1)
    if command == 'shutdown' and mode == 'migration_shutdown_timeout': sys.exit(124)
    if command == 'erase' and mode == 'migration_erase_error': sys.exit(1)
elif command == 'bootstatus':
    assert args[2:] == [uuid, '-b']
    number = count('xcrun', 'simctl', 'bootstatus')
    if mode == 'bootstatus_timeout' or (mode == 'bootstatus_timeout_once' and number == 1) or (mode in ('migration_then_timeout', 'migration_timeout_budget') and number == 2) or (mode == 'migration_timeout_budget' and number == 3):
        print('Status=2, isTerminal=NO\nWaiting on Data Migration'); sys.exit(124)
    if mode == 'bootstatus_error': sys.exit(1)
    if mode in ('migration_failure', 'migration_shutdown_timeout', 'migration_erase_error') or (mode in ('migration_failure_once', 'migration_then_timeout', 'migration_timeout_budget') and number == 1):
        print('Data Migration Failed'); sys.exit(0)
    if mode == 'migration_terminal_once' and number == 1:
        print('Status=3, isTerminal=YES'); sys.exit(0)
    print('Status=4294967295, isTerminal=YES\nFinished')
elif command == 'install': assert args[2] == uuid
elif command == 'get_app_container':
    assert args[2] == uuid
    container = root / 'Devices' / uuid / 'data' / 'container'
    container.mkdir(parents=True, exist_ok=True); print(container)
elif command == 'launch':
    assert args[4] == uuid, args
    shot = pathlib.Path(args[args.index('--screenshot') + 1])
    pixels = bytearray(54 + 320 * 640 * 4); pixels[:2] = b'BM'
    struct.pack_into('<I', pixels, 10, 54)
    struct.pack_into('<iiHH', pixels, 18, 320, 640, 1, 32)
    if mode != 'missing_bmp': shot.write_bytes(pixels[:60] if mode == 'truncated_bmp' else pixels)
    if mode not in ('timeout', 'missing_marker', 'unbuffered_success'): print('org.notelab.editor: 1234')
    if mode == 'timeout': sys.exit(124)
elif command == 'spawn':
    assert args[2] == uuid
    if args[3:5] == ['launchctl', 'list']:
        if mode == 'userspace_timeout' or (mode == 'userspace_timeout_once' and count('xcrun', 'simctl', 'spawn', uuid, 'launchctl', 'list') == 1):
            sys.exit(124)
        print('PID Status Label\n1234 0 UIKitApplication:org.notelab.editor[test]')
    elif args[3:5] == ['log', 'show']:
        if mode != 'missing_marker':
            print('notelab[1234] NoteLab smoke test passed')
        else: print('SpringBoard: unable to finish launching org.notelab.editor')
    else: raise AssertionError(args)
elif command == 'io':
    assert args[2:4] == [uuid, 'screenshot']
    pathlib.Path(args[4]).write_bytes(b'compositor diagnostic, not the required BMP')
else: raise AssertionError(args)
'''


class SimulatorSmokeTests(unittest.TestCase):
    def run_case(self, mode, expected_success, *, launched=True, boots=1, waits=1,
                 erases=0, probes=1, sleeps=(), shutdowns=None):
        with tempfile.TemporaryDirectory(prefix='notelab-ios-smoke-test-') as temporary:
            root = Path(temporary)
            binaries = root / 'bin'; binaries.mkdir()
            temp = root / 'temp'; temp.mkdir()
            app = root / 'notelab.app'; app.mkdir()
            (app / 'Info.plist').write_bytes(plistlib.dumps({
                'CFBundleSupportedPlatforms': ['iPhoneSimulator'],
                'CFBundleIdentifier': 'org.notelab.editor',
                'CFBundleExecutable': 'notelab', 'MinimumOSVersion': '15.0',
            }))
            (app / 'notelab').write_bytes(b'mock executable')
            for tool in ('xcrun', 'ditto', 'codesign', 'sleep'):
                path = binaries / tool
                path.write_text('#!' + sys.executable + '\n' + MOCK)
                path.chmod(0o755)
            output = root / 'output'
            env = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH'],
                       TMPDIR=str(temp), DEVELOPER_DIR=str(root / 'mock-xcode'),
                       NOTELAB_SMOKE_MOCK_ROOT=str(root), NOTELAB_SMOKE_MOCK_MODE=mode)
            result = subprocess.run(['sh', str(Path(__file__).with_name('smoke_simulator.sh')),
                                     str(app), str(output)], env=env, text=True,
                                    capture_output=True, timeout=60)
            self.assertEqual(result.returncode == 0, expected_success, result.stdout + result.stderr)
            self.assertEqual((output / 'result.json').exists(), expected_success)
            calls = [json.loads(line) for line in (root / 'calls.jsonl').read_text().splitlines()]
            simctl = [call[2:] for call in calls if call[:2] == ['xcrun', 'simctl']]
            def count(command):
                return sum(call[0] == command for call in simctl)
            for command in ('install', 'get_app_container', 'launch'):
                self.assertEqual(count(command), int(launched), (command, calls))
            self.assertEqual(count('create'), 1)
            self.assertEqual(count('boot'), boots)
            self.assertEqual(count('bootstatus'), waits)
            self.assertEqual(count('erase'), erases)
            self.assertEqual(count('shutdown'), erases + 1 if shutdowns is None else shutdowns)
            self.assertEqual(simctl[-2:], [['shutdown', UUID], ['delete', UUID]])
            self.assertEqual(sum(call[:2] == ['list', 'devices'] for call in simctl), probes)
            self.assertEqual([call[1:] for call in calls if call[0] == 'sleep'], [[str(delay)] for delay in sleeps])
            for index, call in enumerate(simctl):
                if call[0] == 'erase':
                    self.assertEqual(simctl[index - 1], ['shutdown', UUID])
            for call in simctl:
                self.assertNotIn('booted', call)
                self.assertNotIn('all', call)
                if call[0] in ('boot', 'bootstatus', 'shutdown', 'erase', 'delete', 'install', 'spawn', 'get_app_container'):
                    self.assertEqual(call[1], UUID)
            if not launched:
                self.assertFalse((output / 'launch-state.json').exists())
                self.assertFalse((output / 'smoke.bmp').exists())
                return
            state = json.loads((output / 'launch-state.json').read_text())
            self.assertEqual(state['device_uuid'], UUID)
            if mode not in ('timeout', 'missing_marker', 'unbuffered_success'):
                self.assertEqual(state['log_predicate'], 'processIdentifier == 1234')
                self.assertFalse((output / 'launchctl-after-launch.log').exists())
            else:
                self.assertIsNone(state['launch_pid'])
                self.assertIn('process == "notelab"', state['log_predicate'])
                self.assertIn('eventMessage CONTAINS "org.notelab.editor"', state['log_predicate'])
                self.assertIn('SpringBoard', state['log_predicate'])
                self.assertTrue((output / 'launchctl-after-launch.log').is_file())
                self.assertTrue((output / 'failure-screen.png').is_file())
            if mode == 'timeout':
                # Even a late marker and valid BMP cannot turn timeout into pass.
                self.assertEqual(state['console_exit'], 124)
                self.assertIn('NoteLab smoke test passed', (output / 'unified.log').read_text())
                self.assertTrue((output / 'smoke.bmp').is_file())

    def test_pid_success(self): self.run_case('normal', True)
    def test_no_pid_timeout_stays_failure(self): self.run_case('timeout', False)
    def test_no_pid_missing_marker_stays_failure(self): self.run_case('missing_marker', False)
    def test_no_pid_success_requires_marker_and_bmp(self): self.run_case('unbuffered_success', True)
    def test_missing_bmp_stays_failure(self): self.run_case('missing_bmp', False)
    def test_incomplete_bmp_stays_failure(self): self.run_case('truncated_bmp', False)

    def test_boot_timeout_waits_again_without_reset(self):
        self.run_case('bootstatus_timeout_once', True, waits=2)

    def test_continuous_boot_timeout_never_installs_or_erases(self):
        self.run_case('bootstatus_timeout', False, launched=False, waits=3, probes=0)

    def test_boot_command_error_never_installs_or_erases(self):
        self.run_case('boot_error', False, launched=False, waits=0, probes=0)

    def test_bootstatus_error_never_installs_or_erases(self):
        self.run_case('bootstatus_error', False, launched=False, probes=0)

    def test_migration_failure_resets_only_after_shutdown(self):
        self.run_case('migration_failure_once', True, boots=2, waits=2, erases=1)

    def test_terminal_migration_failure_resets_only_after_shutdown(self):
        self.run_case('migration_terminal_once', True, boots=2, waits=2, erases=1)

    def test_timeout_after_migration_reset_keeps_the_second_boot(self):
        self.run_case('migration_then_timeout', True, boots=2, waits=3, erases=1)

    def test_wait_budget_is_shared_across_migration_resets(self):
        self.run_case('migration_timeout_budget', False, launched=False, boots=2, waits=3, erases=1, probes=0)

    def test_persistent_migration_failure_is_bounded(self):
        self.run_case('migration_failure', False, launched=False, boots=3, waits=3, erases=2, probes=0)

    def test_failed_shutdown_never_erases_or_installs(self):
        self.run_case('migration_shutdown_timeout', False, launched=False, probes=0, shutdowns=2)

    def test_failed_erase_never_boots_again_or_installs(self):
        self.run_case('migration_erase_error', False, launched=False, erases=1, probes=0)

    def test_inventory_timeout_retries_only_readiness(self):
        self.run_case('list_timeout_once', True, probes=2, sleeps=(5,))

    def test_userspace_timeout_retries_only_readiness(self):
        self.run_case('userspace_timeout_once', True, probes=2, sleeps=(5,))

    def test_readiness_failures_never_reset_or_install(self):
        for mode in ('list_timeout', 'userspace_timeout', 'wrong_uuid', 'duplicate_uuid', 'not_booted', 'malformed_state'):
            with self.subTest(mode=mode):
                self.run_case(mode, False, launched=False, probes=3, sleeps=(5, 10))


if __name__ == '__main__':
    unittest.main()
