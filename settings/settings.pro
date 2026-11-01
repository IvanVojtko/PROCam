# SPDX-FileCopyrightText: 2013 - 2021 Jolla Ltd.
# SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = aux
TARGET  = procamsettingsplugin
TARGET = $$qtLibraryTarget($$TARGET)

settingsqml.path = /usr/share/jolla-settings/pages/procam
settingsqml.files = SettingsPage.qml

plugin_entry.path = /usr/share/jolla-settings/entries
plugin_entry.files = procam.json

DEFINES += \
        DEPLOYMENT_PATH=\"\\\"\"$${TARGETPATH}/\"\\\"\"

OTHER_FILES += *.qml

INSTALLS += settingsqml plugin_entry
