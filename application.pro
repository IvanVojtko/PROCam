# SPDX-FileCopyrightText: 2013 - 2021 Jolla Ltd.
# SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = app
TARGET = procam
TARGETPATH = /usr/bin

QT += qml quick
CONFIG += link_pkgconfig

SOURCES += camera.cpp

OTHER_FILES += \
        camera.qml \
        settings.qml \
        cover \
        pages \
        pages/*.qml \
        pages/gallery/*.qml \
        icons/procam-sfos.png \
        dconf/00-procam.txt

target.path = $$TARGETPATH

desktop.path = /usr/share/applications
desktop.files = \
            procam.desktop

icons.path = /usr/share/icons/hicolor/500x500/apps
icons.files = icons/procam-sfos.png

DEPLOYMENT_PATH = /usr/share/$$TARGET
DEFINES *= DEPLOYMENT_PATH=\"\\\"\"$${DEPLOYMENT_PATH}/\"\\\"\"
qml.path = $$DEPLOYMENT_PATH
qml.files = *.qml cover pages

service.files = com.procam.camera.service
service.path  = /usr/share/dbus-1/services

oneshot.files = camera-enable-hints
oneshot.path  = /usr/lib/oneshot.d

schema.files = dconf/00-procam.txt
schema.path  = /etc/dconf/db/vendor.d/

INSTALLS += target desktop icons qml service schema

packagesExist(qdeclarative5-boostable) {
    message("Building with qdeclarative-boostable support")
    DEFINES += HAS_BOOSTER
    PKGCONFIG += qdeclarative5-boostable
} else {
    warning("qdeclarative-boostable not available; startup times will be slower")
}
