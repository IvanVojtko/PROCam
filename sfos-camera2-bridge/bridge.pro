# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = subdirs
CONFIG += no_strip
QMAKE_STRIP = :

SUBDIRS = sailfish

android_bridge.files = build/android/libprocamcamera2.so
android_bridge.path = /usr/libexec/droid-hybris/system/lib64

INSTALLS += android_bridge

OTHER_FILES += \
    android/camera2_bridge.c \
    android/camera2_bridge.h \
    android/camera2_common.c \
    android/camera2_common.h \
    android/jpeg_capture.c \
    android/video_record.c \
    android/recorder.c \
    android/recorder.h \
    android/heif_encode.c \
    android/preview.c \
    android/raw_capture.c \
    scripts/build-android.sh \
    scripts/build-sailfish.sh
