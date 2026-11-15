#!/bin/sh
set -eu
platform_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
editor_dir=$(CDPATH= cd -- "$platform_dir/../.." && pwd)
target=${1:-simulator}
if [ "$#" -gt 0 ]; then shift; fi
case "$target" in
  simulator) sdk=iphonesimulator; architecture=$(uname -m) ;;
  device) sdk=iphoneos; architecture=arm64 ;;
  *) echo "Usage: $0 [simulator|device] [extra CMake configure arguments]" >&2; exit 2 ;;
esac
# Select an existing Xcode only for this process; do not change xcode-select.
if [ -z "${DEVELOPER_DIR:-}" ] && [ -d /Applications/Xcode.app/Contents/Developer ]; then
  DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  export DEVELOPER_DIR
fi
if ! xcrun --sdk "$sdk" --show-sdk-path >/dev/null; then
  echo "Install full Xcode with the $sdk SDK, then set DEVELOPER_DIR if needed." >&2
  exit 1
fi
build_dir=${NOTELAB_IOS_BUILD_DIR:-"$editor_dir/build-ios-$target"}
cmake -S "$platform_dir" -B "$build_dir" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk" \
  -DCMAKE_OSX_ARCHITECTURES="$architecture" -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -DBUILD_TESTING=OFF "$@"
cmake --build "$build_dir" --config Release --target notelab -- -quiet CODE_SIGNING_ALLOWED=NO
printf 'Unsigned app: %s/notelab/Release-%s/notelab.app\n' "$build_dir" "$sdk"
