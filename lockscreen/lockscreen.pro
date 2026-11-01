# SPDX-FileCopyrightText: 2016 - 2018 Jolla Ltd.
# SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = app
TARGET = procam-lockscreen
TARGETPATH = /usr/bin

QT += qml quick
CONFIG += link_pkgconfig

SOURCES += main.cpp

OTHER_FILES += \
        *.qml

target.path = $$TARGETPATH

DEPLOYMENT_PATH = /usr/share/procam
DEFINES *= DEPLOYMENT_PATH=\"\\\"\"$${DEPLOYMENT_PATH}/\"\\\"\"
qml.path = $$DEPLOYMENT_PATH
qml.files = *.qml

INSTALLS += target qml

packagesExist(qdeclarative5-boostable) {
    message("Building with qdeclarative-boostable support")
    DEFINES += HAS_BOOSTER
    PKGCONFIG += qdeclarative5-boostable
} else {
    warning("qdeclarative-boostable not available; startup times will be slower")
}

