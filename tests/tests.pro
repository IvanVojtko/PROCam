# SPDX-FileCopyrightText: 2013 - 2014 Jolla Ltd.
# SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = subdirs

OTHER_FILES += auto/* exifmetadata.cpp exifmetadata.pro

auto.files = auto/*
auto.path = /opt/tests/procam/auto

definition.files = test-definition/tests.xml
definition.path = /opt/tests/procam/test-definition

INSTALLS += auto definition
