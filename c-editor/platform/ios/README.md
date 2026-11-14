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
an available compatible iOS runtime and iPhone/iPad device type, creates a new
temporary simulator, and installs a temporary copy of the simulator bundle. It
adds an ad-hoc signature to that copy only when required. Notes and the screenshot
are written inside this installation's private `Documents` directory. Each
simulator command has a timeout; boot readiness allows 180 seconds and the app
allows 90 seconds. Cleanup shuts down and deletes only the UUID created by this
run, including after failure.

A passing run requires the C app's success message, a successful console launch
exit, and a complete BMP screenshot. Preserve `app.log`, `simulator.log`,
`selection.json`, `result.json`, and `smoke.bmp` from the output directory as CI
artifacts. The script's success and failure cleanup paths have been checked with
a simulated command runner; a real simulator run remains unverified locally due
to the CoreSimulator mismatch described above. This startup check does not cover
touch input, IME, background persistence, or hardware behavior.

References: [SDL iOS guide](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/docs/README-ios.md)
and [CMake iOS cross compilation](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#cross-compiling-for-ios-tvos-visionos-or-watchos).
