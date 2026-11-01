# SPDX-FileCopyrightText: 2013 - 2017 Jolla Ltd.
# SPDX-FileCopyrightText: 2024 - 2025 Jolla Mobile Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = subdirs

SUBDIRS = \
        application.pro \
        src \
        settings/settings.pro \
        sfos-camera2-bridge/bridge.pro

OTHER_FILES = rpm/*.spec
