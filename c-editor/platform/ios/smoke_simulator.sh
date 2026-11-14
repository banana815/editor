#!/bin/sh
# Install and render on a disposable simulator. Never use the "booted" alias.
set -eu
platform_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
editor_dir=$(CDPATH= cd -- "$platform_dir/../.." && pwd)
app=${1:-"$editor_dir/build-ios-simulator/notelab/Release-iphonesimulator/notelab.app"}
output=${2:-"$editor_dir/build-ios-simulator/smoke-output"}
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/notelab-ios-smoke.XXXXXX")
device_uuid=
log="$output/simulator.log"
: > "$log"
rm -f -- "$output/result.json" "$output/smoke.bmp" "$output/selection.json" "$output/app.log" \
  "$output/boot-1.log" "$output/boot-2.log" "$output/boot-3.log"

# macOS does not ship GNU timeout. Bound each child command and kill only that
# command's process group on timeout; Simulator itself is cleaned up by UUID.
run_limited() {
  python3 - "$@" <<'PY'
import os, shlex, signal, subprocess, sys
seconds = int(sys.argv[1])
assert 0 < seconds <= 600
print('+ ' + shlex.join(sys.argv[2:]), file=sys.stderr, flush=True)
process = subprocess.Popen(sys.argv[2:], stdin=subprocess.DEVNULL, start_new_session=True)
try:
    code = process.wait(timeout=seconds)
except subprocess.TimeoutExpired:
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()
    print(f'Timed out after {seconds}s: ' + shlex.join(sys.argv[2:]), file=sys.stderr)
    code = 124
sys.exit(code if code >= 0 else 128 - code)
PY
}

cleanup() {
  saved_status=$?
  trap - EXIT INT TERM
  if [ -n "$device_uuid" ]; then
    printf '\nCleaning up this run\047s simulator: %s\n' "$device_uuid" >> "$log"
    run_limited 30 xcrun simctl shutdown "$device_uuid" >> "$log" 2>&1 || true
    run_limited 30 xcrun simctl delete "$device_uuid" >> "$log" 2>&1 || true
  fi
  if [ "$saved_status" -ne 0 ]; then
    tail -n 100 "$log" >&2
  fi
  rm -rf -- "$work_dir"
  exit "$saved_status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [ -z "${DEVELOPER_DIR:-}" ] && [ -d /Applications/Xcode.app/Contents/Developer ]; then
  DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  export DEVELOPER_DIR
fi

# Validate the input bundle before signing or creating any simulator.
python3 - "$app" "$work_dir" <<'PY'
import pathlib, plistlib, re, sys
app, work = map(pathlib.Path, sys.argv[1:])
with (app / 'Info.plist').open('rb') as handle:
    info = plistlib.load(handle)
if 'iPhoneSimulator' not in info.get('CFBundleSupportedPlatforms', []):
    raise SystemExit('A simulator .app is required; device apps will not be signed or installed.')
bundle = info.get('CFBundleIdentifier', '')
executable = info.get('CFBundleExecutable', '')
if not re.fullmatch(r'[A-Za-z0-9.-]+', bundle) or pathlib.Path(executable).name != executable or not executable:
    raise SystemExit('Invalid bundle identifier or executable name')
if not (app / executable).is_file():
    raise SystemExit('The application executable is missing')
(work / 'bundle-id').write_text(bundle)
(work / 'executable').write_text(executable)
(work / 'minimum-os').write_text(info.get('MinimumOSVersion', '15.0'))
PY
bundle_id=$(cat "$work_dir/bundle-id")
executable=$(cat "$work_dir/executable")
run_limited 30 xcrun vtool -show-build "$app/$executable" > "$work_dir/binary-platform" 2>> "$log"
python3 - "$work_dir/binary-platform" <<'PY'
import pathlib, re, sys
if not re.search(r'platform\s+IOSSIMULATOR\b', pathlib.Path(sys.argv[1]).read_text()):
    raise SystemExit('The executable is not an iOS simulator binary.')
PY

# The first simctl call can spend minutes mounting installed runtime images.
run_limited 180 xcrun simctl list --json > "$work_dir/inventory.json" 2>> "$log"
python3 - "$work_dir" "$output" <<'PY'
import json, pathlib, re, sys
work, output = map(pathlib.Path, sys.argv[1:])
inventory = json.loads((work / 'inventory.json').read_text())
def version(text):
    numbers = [int(x) for x in re.findall(r'\d+', str(text))[:3]]
    return tuple((numbers + [0, 0, 0])[:3])
minimum = version((work / 'minimum-os').read_text())
runtimes = [r for r in inventory.get('runtimes', [])
            if r.get('isAvailable') and '.SimRuntime.iOS-' in r.get('identifier', '')
            and version(r.get('version', '0')) >= minimum]
runtimes.sort(key=lambda r: version(r['version']), reverse=True)
selected = None
for runtime in runtimes:
    current = version(runtime['version'])
    supported = {d.get('identifier') if isinstance(d, dict) else d
                 for d in runtime.get('supportedDeviceTypes', [])}
    choices = []
    for device in inventory.get('devicetypes', []):
        if device.get('productFamily') not in ('iPhone', 'iPad'):
            continue
        if supported and device['identifier'] not in supported:
            continue
        if version(device.get('minRuntimeVersionString', '0')) > current:
            continue
        if version(device.get('maxRuntimeVersionString', '65535.255.255')) < current:
            continue
        choices.append(device)
    # Prefer established standard-size iPhones over Air/Pro/Max templates.
    # Every choice still has to match the discovered runtime's compatibility.
    choices.sort(key=lambda d: (d.get('productFamily') == 'iPhone',
                               d['name'] in ('iPhone 16', 'iPhone 15'),
                               bool(re.fullmatch(r'iPhone \d+', d['name'])),
                               version(d.get('minRuntimeVersionString', '0')), d['name']), reverse=True)
    if choices:
        selected = {'runtime': runtime['identifier'], 'runtime_name': runtime['name'],
                    'device_type': choices[0]['identifier'], 'device_name': choices[0]['name']}
        break
if selected is None:
    raise SystemExit('No available iOS runtime/device type matches this app. Install a simulator runtime in Xcode.')
for field in ('runtime', 'device_type'):
    if not re.fullmatch(r'[A-Za-z0-9.-]+', selected[field]):
        raise SystemExit('Unexpected simulator identifier')
    (work / field).write_text(selected[field])
(output / 'selection.json').write_text(json.dumps(selected, indent=2) + '\n')
print('Selected:', selected['runtime_name'], '/', selected['device_name'])
PY
runtime=$(cat "$work_dir/runtime")
device_type=$(cat "$work_dir/device_type")

# Preserve the original unsigned build artifact. Add an ad-hoc simulator-only
# signature to a temporary copy only when verification says it needs one.
run_limited 45 ditto "$app" "$work_dir/notelab.app" >> "$log" 2>&1
if ! run_limited 30 codesign --verify --deep --strict "$work_dir/notelab.app" >> "$log" 2>&1; then
  run_limited 30 codesign --force --sign - --timestamp=none "$work_dir/notelab.app" >> "$log" 2>&1
  run_limited 30 codesign --verify --deep --strict "$work_dir/notelab.app" >> "$log" 2>&1
fi

created_uuid=$(run_limited 45 xcrun simctl create "NoteLab-smoke-$(basename "$work_dir")" "$device_type" "$runtime" 2>> "$log")
device_uuid=$(python3 - "$created_uuid" <<'PY'
import sys, uuid
print(str(uuid.UUID(sys.argv[1].strip())).upper())
PY
)
printf 'Created simulator: %s\n' "$device_uuid" | tee -a "$log"

boot_ready() {
  run_limited 45 xcrun simctl boot "$device_uuid" || return 1
  run_limited 180 xcrun simctl bootstatus "$device_uuid" -b || return 1
  # Some CoreSimulator versions return zero even after data migration fails.
  # A Booted device alone does not establish successful first-boot migration.
  python3 - "$output/boot-$attempt.log" <<'PY' || return 1
import pathlib, re, sys
log = pathlib.Path(sys.argv[1]).read_text(errors='replace')
if 'Data Migration Failed' in log or re.search(r'Status=3,\s*isTerminal=YES', log):
    raise SystemExit('Simulator data migration failed; refusing to install the app')
PY
  run_limited 60 xcrun simctl list devices --json > "$work_dir/device-state.json" || return 1
  python3 - "$work_dir/device-state.json" "$device_uuid" <<'PY' || return 1
import json, pathlib, sys
devices = json.loads(pathlib.Path(sys.argv[1]).read_text()).get('devices', {})
matches = [d for group in devices.values() for d in group
           if d.get('udid', '').lower() == sys.argv[2].lower()]
if len(matches) != 1 or matches[0].get('state') != 'Booted':
    raise SystemExit('The newly created simulator is not Booted')
PY
  # Confirm that this simulator's userspace responds before installing anything.
  run_limited 60 xcrun simctl spawn "$device_uuid" launchctl list > "$work_dir/launchd-status"
}

attempt=1
ready=false
while [ "$attempt" -le 3 ]; do
  if [ "$attempt" -gt 1 ]; then
    # Only the empty simulator created above is reset, never a pre-existing one.
    run_limited 30 xcrun simctl shutdown "$device_uuid" >> "$log" 2>&1 || true
    run_limited 60 xcrun simctl erase "$device_uuid" >> "$log" 2>&1
  fi
  printf 'Boot readiness attempt %s of 3\n' "$attempt" | tee -a "$log"
  if boot_ready > "$output/boot-$attempt.log" 2>&1; then
    cat "$output/boot-$attempt.log" >> "$log"
    ready=true
    break
  fi
  cat "$output/boot-$attempt.log" >> "$log"
  attempt=$((attempt + 1))
done
if [ "$ready" != true ]; then
  echo 'Simulator did not become ready after three bounded boot attempts' >&2
  exit 1
fi
run_limited 120 xcrun simctl install "$device_uuid" "$work_dir/notelab.app" >> "$log" 2>&1
container=$(run_limited 120 xcrun simctl get_app_container "$device_uuid" "$bundle_id" data 2>> "$log")
python3 - "$container" "$device_uuid" "$work_dir" <<'PY'
import json, pathlib, sys
container = pathlib.Path(sys.argv[1]).resolve(strict=True)
device = sys.argv[2]
work = pathlib.Path(sys.argv[3])
if device.lower() not in [part.lower() for part in container.parts]:
    raise SystemExit('Refusing a data container outside the simulator created by this run')
notes = container / 'Documents' / ('NoteLab-smoke-' + work.name)
notes.mkdir(parents=True, exist_ok=False)
fixture = {'type': 'doc', 'content': [
    {'type': 'heading', 'attrs': {'level': 2}, 'content': [{'type': 'text', 'text': 'iOS 启动测试 📝'}]},
    {'type': 'paragraph', 'content': [{'type': 'text', 'text': 'C editor simulator smoke test', 'marks': [{'type': 'bold'}]}]}
]}
(notes / 'smoke.json').write_text(json.dumps(fixture, ensure_ascii=False), encoding='utf-8')
(work / 'notes-dir').write_text(str(notes))
PY
notes_dir=$(cat "$work_dir/notes-dir")
screenshot="$notes_dir/smoke.bmp"
if run_limited 90 xcrun simctl launch --console --terminate-running-process "$device_uuid" "$bundle_id" \
    --smoke-test --notes "$notes_dir" --screenshot "$screenshot" > "$output/app.log" 2>&1; then
  cat "$output/app.log"
else
  cat "$output/app.log" >&2
  echo "Simulator launch failed or timed out; see $output" >&2
  exit 1
fi
python3 - "$screenshot" "$output" "$device_uuid" <<'PY'
import json, pathlib, shutil, struct, sys
shot, output = map(pathlib.Path, sys.argv[1:3])
data = shot.read_bytes()
if 'NoteLab smoke test passed' not in (output / 'app.log').read_text(errors='replace'):
    raise SystemExit('The C app did not report successful completion of its smoke test')
if len(data) < 54 or data[:2] != b'BM':
    raise SystemExit('The app did not produce a valid BMP screenshot')
pixel_offset = struct.unpack_from('<I', data, 10)[0]
width, height, planes, bits = struct.unpack_from('<iiHH', data, 18)
if width <= 0 or height == 0 or planes != 1 or bits not in (24, 32):
    raise SystemExit('Unexpected screenshot dimensions or pixel format')
stride = ((width * bits + 31) // 32) * 4
if pixel_offset < 54 or len(data) < pixel_offset + stride * abs(height):
    raise SystemExit('Screenshot pixels are incomplete')
shutil.copyfile(shot, output / 'smoke.bmp')
result = json.loads((output / 'selection.json').read_text())
result.update(device_uuid=sys.argv[3], width=width, height=abs(height),
              result='C app reported smoke success, launch exited successfully, and BMP is complete')
(output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(f'iOS simulator smoke passed: {width}x{abs(height)}; artifacts: {output}')
PY
