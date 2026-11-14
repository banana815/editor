# Android build

This Gradle project packages the shared C editor with SDL's Java lifecycle bridge.
It is a build entry point, not a claim of Android runtime or feature parity.

Requirements: Python 3.12+, curl, JDK 17, Android command-line tools, SDK platform
35, build tools 35.0.0, NDK 28.2.13676358, and SDK CMake 3.31.6. No Node or Rust
runtime is used. The wrapper pins Gradle 8.12 with a SHA-256 check; Android Gradle
Plugin 8.7.3 matches the pinned SDL release's template.

From this directory:

```sh
sdkmanager 'platforms;android-35' 'build-tools;35.0.0' 'ndk;28.2.13676358' 'cmake;3.31.6'
python3 prepare.py
./gradlew :app:assembleRelease
```

Set `ANDROID_HOME` to an existing SDK, or create an untracked `local.properties`
containing `sdk.dir=/absolute/path/to/sdk`. The output is
`app/build/outputs/apk/release/app-release-unsigned.apk`; no release signing keys
are read or created. For a locally installable test package use
`./gradlew :app:assembleDebug` (Android's standard local debug signature).

The default ABIs are `arm64-v8a,x86_64`; override with e.g. `-Pabis=arm64-v8a`.
`prepare.py` downloads and verifies the exact SDL source pinned in the shared
`cmake/Dependencies.cmake`, so Java and native SDL cannot silently diverge. It
keeps that source under the ignored `c-editor/build-android-deps/SDL`. An existing
matching source checkout can instead be passed as `-PsdlSource=/absolute/path`.

Gradle invokes the CMake entry point in `app/`, which builds `libmain.so`, SDL3,
SDL3_ttf and their dependencies. The CMake-generated Noto font and OFL license are
staged per ABI before asset merging and appear as `assets/font.otf` and
`assets/OFL.txt`. C++ is only used by native dependencies such as HarfBuzz.
Notes use SDL's private application storage. No external-storage permission is
requested; OS file sharing/import integration is a separate unfinished feature.

Validation on the current development machine: project XML, preparation script,
and downloaded SDL source were checked. No Android SDK/NDK is installed, so the
APK build and device interaction remain unverified. The sibling
The Android job in `.github/workflows/c-editor.yml` compiles and checks the APK contents in CI.

References: [SDL Android guide](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/docs/README-android.md).
The Java/native bridge follows that pinned SDL source (`SDL-LICENSE.txt`). The
wrapper and launchers come from Gradle's official `v8.12.0` tag; see
`GRADLE-LICENSE.txt`. The wrapper JAR was checked against Gradle's published SHA-256:
`2db75c40782f5e8ba1fc278a5574bab070adccb2d21ca5a6e5ed840888448046`.
The older wrapper in SDL's template is intentionally not used, so the configured
distribution checksum is enforced.
