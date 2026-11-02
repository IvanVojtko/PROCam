# SPDX-FileCopyrightText: 2014 - 2021 Jolla Ltd.
# SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = lib
TARGET  = vividcameraplugin
TARGET = $$qtLibraryTarget($$TARGET)

MODULENAME = com/procam/camera
TARGETPATH = $$[QT_INSTALL_QML]/$$MODULENAME

QT += gui-private qml quick multimedia concurrent
CONFIG += plugin link_pkgconfig c++14

PKGCONFIG += mlite5 systemsettings libtiff-4 libexif

# The RAW pipeline is long, vectorizable loops: -O3 vectorizes them fully
# (GCC's -O2 only takes the cheapest cases) and maths functions that never
# set errno can vectorize too. NaN/Inf checks stay valid (no -ffast-math).
# Appended after the SDK's -O2, so it wins. About 11% faster per burst on
# the host benchmark, a third faster merge.
QMAKE_CXXFLAGS_RELEASE += -O3 -fno-math-errno

SOURCES += rawbracket.cpp \
        rawpipeline.cpp \
        rawpipelinemath.cpp \
        raisr.cpp \
        camera2preview.cpp \
        previewnode.cpp \
        cameraplugin.cpp \
        capturemodel.cpp \
        declarativecameraextensions.cpp \
        exifutils.cpp \
        dnglensshading.cpp \
        imageadjustments.cpp \
        declarativesettings.cpp \
        cameraconfigs.cpp

HEADERS += rawbracket.h rawbracketmath.h \
        rawpipeline.h rawpipelinemath.h raisr.h \
        previewsize.h previewaids.h videoencoderlimits.h \
        camera2preview.h \
        previewnode.h \
        capturemodel.h \
        declarativecameraextensions.h \
        exifutils.h \
        dnglensshading.h \
        imageadjustments.h \
        declarativesettings.h \
        cameraconfigs.h

DEFINES += \
        DEPLOYMENT_PATH=\"\\\"\"$${TARGETPATH}/\"\\\"\"

import.files = \
        DisabledByMdmView.qml \
        CameraPage.qml \
        capture \
        gallery \
        qmldir \
        settings \
        settings.qml \
        calibration

import.path = $$TARGETPATH
target.path = $$TARGETPATH

INSTALLS += target import

OTHER_FILES = \
        DisabledByMdmView.qml \
        CameraPage.qml \
        capture/*.qml \
        capture/*.js \
        gallery/*.qml \
        settings/*.qml \
        settings/*.js \
        settings.qml \
        qmldir
