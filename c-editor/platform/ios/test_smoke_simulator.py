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
with (root / 'calls.jsonl').open('a') as log:
    log.write(json.dumps([tool, *args]) + '\n')
if tool == 'ditto':
    shutil.copytree(args[0], args[1]); sys.exit(0)
if tool == 'codesign': sys.exit(0)
assert tool == 'xcrun', tool
if args[0] == 'vtool': print('platform IOSSIMULATOR'); sys.exit(0)
assert args[0] == 'simctl', args
command = args[1]
if command == 'list':
    if 'devices' in args:
        print(json.dumps({'devices': {'runtime': [{'udid': uuid, 'state': 'Booted'}]}}))
    else:
        print(json.dumps({'runtimes': [{'isAvailable': True, 'identifier': 'com.apple.CoreSimulator.SimRuntime.iOS-99-1', 'version': '99.1', 'name': 'Mock iOS'}],
                          'devicetypes': [{'identifier': 'com.apple.CoreSimulator.SimDeviceType.iPhone-16', 'name': 'iPhone 16', 'productFamily': 'iPhone'}]}))
elif command == 'create': print(uuid)
elif command in ('boot', 'shutdown', 'delete', 'erase'):
    assert args[2] == uuid
elif command == 'bootstatus':
    assert args[2] == uuid; print('Status=4294967295, isTerminal=YES\nFinished')
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
    shot.write_bytes(pixels)
    if os.environ['NOTELAB_SMOKE_MOCK_MODE'] == 'normal': print('org.notelab.editor: 1234')
    if os.environ['NOTELAB_SMOKE_MOCK_MODE'] == 'timeout': sys.exit(124)
elif command == 'spawn':
    assert args[2] == uuid
    if args[3:5] == ['launchctl', 'list']:
        print('PID Status Label\n1234 0 UIKitApplication:org.notelab.editor[test]')
    elif args[3:5] == ['log', 'show']:
        if os.environ['NOTELAB_SMOKE_MOCK_MODE'] != 'missing_marker':
            print('notelab[1234] NoteLab smoke test passed')
        else: print('SpringBoard: unable to finish launching org.notelab.editor')
    else: raise AssertionError(args)
elif command == 'io':
    assert args[2:4] == [uuid, 'screenshot']
    pathlib.Path(args[4]).write_bytes(b'compositor diagnostic, not the required BMP')
else: raise AssertionError(args)
'''


class SimulatorSmokeTests(unittest.TestCase):
    def run_case(self, mode, expected_success):
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
            for tool in ('xcrun', 'ditto', 'codesign'):
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
            state = json.loads((output / 'launch-state.json').read_text())
            self.assertEqual(state['device_uuid'], UUID)
            calls = [json.loads(line) for line in (root / 'calls.jsonl').read_text().splitlines()]
            simctl = [call[2:] for call in calls if call[:2] == ['xcrun', 'simctl']]
            self.assertEqual(sum(call[0] == 'launch' for call in simctl), 1)
            self.assertEqual([call for call in simctl if call[0] in ('shutdown', 'delete')],
                             [['shutdown', UUID], ['delete', UUID]])
            for call in simctl:
                self.assertNotIn('booted', call)
                self.assertNotIn('all', call)
            if mode == 'normal':
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


if __name__ == '__main__':
    unittest.main()
