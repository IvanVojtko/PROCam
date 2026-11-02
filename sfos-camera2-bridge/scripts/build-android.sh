#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ndk_dir=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}

if [ -z "$ndk_dir" ]; then
    echo "Set ANDROID_NDK_HOME to the Android NDK directory." >&2
    exit 2
fi

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) host_tag=linux-x86_64 ;;
    Darwin-x86_64) host_tag=darwin-x86_64 ;;
    Darwin-arm64) host_tag=darwin-x86_64 ;;
    *)
        echo "Unsupported NDK host: $(uname -s)-$(uname -m)" >&2
        exit 3
        ;;
esac

compiler="$ndk_dir/toolchains/llvm/prebuilt/$host_tag/bin/aarch64-linux-android24-clang"
if [ ! -x "$compiler" ]; then
    echo "Android NDK compiler not found: $compiler" >&2
    exit 4
fi

output_dir="$project_dir/build/android"
mkdir -p "$output_dir"

"$compiler" \
    -std=c11 -O2 -g -fPIC -fvisibility=hidden -pthread \
    -Wall -Wextra -Werror \
    -shared -Wl,--no-undefined -Wl,--build-id=sha1 -Wl,-soname,libprocamcamera2.so \
    "$project_dir/android/camera2_bridge.c" \
    "$project_dir/android/camera2_common.c" \
    "$project_dir/android/raw_capture.c" \
    "$project_dir/android/preview.c" \
    "$project_dir/android/jpeg_capture.c" \
    "$project_dir/android/video_record.c" \
    "$project_dir/android/recorder.c" \
    -I"$project_dir/android" \
    -lcamera2ndk -lmediandk -landroid -lm \
    -o "$output_dir/libprocamcamera2.so"

echo "Built $output_dir/libprocamcamera2.so"
