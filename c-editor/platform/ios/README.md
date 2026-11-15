# iPhone and iPad build

The wrapper builds the same C sources and static SDL dependencies as the desktop
editor. It supplies an iOS bundle manifest, iPhone/iPad device families, rotation
support, and a launch screen. Noto font assets and the OFL license come from the
shared CMake build and are bundled in `notelab.app/assets`.

Requirements: macOS, full Xcode with the iOS SDK, and CMake 3.24+. Command Line
Tools alone do not include the mobile SDK. The script uses an existing
`/Applications/Xcode.app` when available; an explicit `DEVELOPER_DIR` selects
another installation without changing the system's `xcode-select` setting.

From the repository root:

```sh
sh c-editor/platform/ios/build.sh simulator
sh c-editor/platform/ios/build.sh device
```

Both commands disable code signing and never archive or publish. The simulator
build targets the host architecture; the device build targets arm64. The minimum
OS is iOS 15.0. A device build is compile evidence only: installing it on hardware
requires a separately configured development team and signing.

Outputs are `c-editor/build-ios-simulator/notelab/Release-iphonesimulator/notelab.app`
and `c-editor/build-ios-device/notelab/Release-iphoneos/notelab.app`. The generated
Xcode project is also in the corresponding build directory. Override the output
root with `NOTELAB_IOS_BUILD_DIR`. Additional arguments go to CMake, for example:

```sh
sh c-editor/platform/ios/build.sh simulator -DEDITOR_FONT_PATH=/path/to/font.otf
```

The application writes notes to SDL's writable private directory, not the
read-only application bundle. A successful compile alone does not verify touch,
IME input, background saves, file sharing, or Zotero integration on iOS; those
need simulator/device interaction separately.

The native Files picker accepts old notes as JSON, HTML/HTM, Markdown, or plain
text. Zotero import also accepts SQLite snapshots; the C importer validates file
contents after selection. The picker filter policy has a small macOS-hosted test
using the same Uniform Type Identifiers framework, without starting a simulator:

```sh
xcrun clang -fobjc-arc -Wall -Wextra -Werror \
  -I c-editor/include -I c-editor/build-web/_deps/editor_sdl-src/include \
  c-editor/platform/ios/test_import_types.m \
  -framework Foundation -framework UniformTypeIdentifiers \
  -o /tmp/notelab-import-types
/tmp/notelab-import-types
```

Point the SDL include path at the SDL source checkout used by your build. This
checks that each supported extension can be selected and that the JSON-only
picker remains restricted; actual provider and picker interaction is separate.

Validation on 2026-11-14: both commands completed with Xcode 27.2 / SDK 27.2 on
Apple Silicon. The resulting arm64 binaries report `IOSSIMULATOR` and `IOS`
respectively, deployment target 15.0. Both bundles contain the verified Noto font
and OFL license. The local CoreSimulator service was older than the installed
Xcode, so simulator launch and hardware interaction were not verified.

To check installation, startup, rendering, and clean exit on an installed iOS
simulator runtime, run:

```sh
sh c-editor/platform/ios/smoke_simulator.sh \
  c-editor/build-ios-simulator/notelab/Release-iphonesimulator/notelab.app \
  c-editor/build-ios-simulator/smoke-output
```

Both arguments are optional and default to the paths above. The script discovers
an available compatible iOS runtime and iPhone/iPad device type, preferring a
standard iPhone 16/15 when compatible, creates a new
temporary simulator, and installs a temporary copy of the simulator bundle. It
adds an ad-hoc signature to that copy only when required. Notes and the screenshot
are written inside this installation's private `Documents` directory. Each
simulator command has a timeout; initial runtime discovery and each boot readiness
check allow 180 seconds, installation/container lookup allow 120 seconds, and the
app allows 90 seconds. Readiness rejects failed data migration even when `simctl`
returns zero, checks the new device's state, and probes its running userspace.
Only this empty temporary device may be shut down, erased, and booted again, up
to two times. Application failures are not retried. Cleanup shuts down and deletes
only the UUID created by this run, including after failure.

A passing run requires the C app's success message, a successful console launch
exit, and a complete BMP screenshot. Preserve `app.log`, `unified.log`, `simulator.log`,
`boot-*.log`, `selection.json`, `launch-state.json`, `result.json`, and `smoke.bmp`
from the output directory as CI artifacts. `unified.log` captures the C app's
NSLog messages when they are not forwarded to the attached console. When simctl
has not returned a PID, the query stays on this run's UUID and selects the app
process plus bundle-related messages from SpringBoard, RunningBoard and launch
services. A missing PID can also mean buffered console output, so it is not
treated as proof that no process started. A failed launch or missing PID also
records `launchctl-after-launch.log` and, when available, the simulator compositor's
`failure-screen.png`. That diagnostic PNG cannot replace the C renderer's BMP or
make a timed-out launch pass. No application launch is retried.

Run the command-isolation and result-gating tests without Xcode or a simulator:

```sh
python3 c-editor/platform/ios/test_smoke_simulator.py
```

These tests replace Apple tools with temporary mock executables and verify that
only the newly created UUID is inspected/cleaned up, that only one launch occurs,
and that timeout or missing success messages still fail even with diagnostic
output. A screenshot may also be kept after failure
for diagnosis; only `result.json` denotes a passing check. The script's success and failure cleanup paths have been checked with
a simulated command runner; a real simulator run remains unverified locally due
to the CoreSimulator mismatch described above. This startup check does not cover
touch input, IME, background persistence, or hardware behavior.

References: [SDL iOS guide](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/docs/README-ios.md)
and [CMake iOS cross compilation](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#cross-compiling-for-ios-tvos-visionos-or-watchos).
