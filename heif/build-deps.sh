#!/bin/sh
# SPDX-FileCopyrightText: 2026 ProCam Contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# build-deps.sh SOURCE_DIR PREFIX: builds libde265 and libheif (decoding
# only, static, position independent) from the third_party submodules and
# installs them under PREFIX for the HEIF image plugin.
set -eu

source_dir=$1
prefix=$2
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
# The real cmake: the Sailfish SDK puts a wrapper for its own shadow builds
# first in PATH, which only takes "cmake --build .".
cmake=cmake
[ -x /usr/bin/cmake ] && cmake=/usr/bin/cmake

if [ ! -f "$source_dir/libde265/CMakeLists.txt" ] || [ ! -f "$source_dir/libheif/CMakeLists.txt" ]; then
    echo "libde265 or libheif sources missing: git submodule update --init" >&2
    exit 1
fi

common="-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$prefix -DCMAKE_INSTALL_LIBDIR=lib
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_C_VISIBILITY_PRESET=hidden -DCMAKE_CXX_VISIBILITY_PRESET=hidden
        -DCMAKE_PREFIX_PATH=$prefix"

# shellcheck disable=SC2086
"$cmake" -S "$source_dir/libde265" -B "$prefix/build/libde265" $common \
    -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF -DENABLE_SDL=OFF
"$cmake" --build "$prefix/build/libde265" -j "$jobs"
"$cmake" --install "$prefix/build/libde265"

# Only the libde265 (HEVC) decoder: no encoders, AV1, JPEG 2000 or plugins.
# shellcheck disable=SC2086
PKG_CONFIG_PATH="$prefix/lib/pkgconfig" "$cmake" -S "$source_dir/libheif" -B "$prefix/build/libheif" $common \
    -DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF \
    -DWITH_X265=OFF -DWITH_KVAZAAR=OFF -DWITH_UVG266=OFF -DWITH_VVDEC=OFF -DWITH_VVENC=OFF \
    -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF -DWITH_RAV1E=OFF \
    -DWITH_SvtEnc=OFF -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF \
    -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF -DWITH_OPENJPH_ENCODER=OFF \
    -DWITH_OPENJPH_DECODER=OFF -DWITH_FFMPEG_DECODER=OFF -DWITH_UNCOMPRESSED_CODEC=OFF \
    -DWITH_LIBSHARPYUV=OFF -DWITH_HEADER_COMPRESSION=OFF -DENABLE_PLUGIN_LOADING=OFF \
    -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF -DWITH_REDUCED_VISIBILITY=ON -DBUILD_TESTING=OFF
"$cmake" --build "$prefix/build/libheif" -j "$jobs"
"$cmake" --install "$prefix/build/libheif"
