# SPDX-FileCopyrightText: 2026 ProCam Contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# Qt image format plugin for HEIC/HEIF photos (read only). libde265 and
# libheif (LGPL-3.0, third_party submodules) are built by build-deps.sh and
# linked in statically, so nothing clashes with other copies on the phone.

TEMPLATE = lib
CONFIG += plugin c++17
QT = core gui
TARGET = procamheif

DEPS = $$OUT_PWD/deps
heifdeps.target = $$DEPS/lib/libheif.a
heifdeps.commands = sh $$PWD/build-deps.sh $$PWD/../third_party $$DEPS
# The plugin's object needs the headers, not only its link the libraries.
heifobject.target = heifplugin.o
heifobject.depends = $$DEPS/lib/libheif.a
QMAKE_EXTRA_TARGETS += heifdeps heifobject
PRE_TARGETDEPS += $$DEPS/lib/libheif.a

INCLUDEPATH += $$DEPS/include
DEFINES += LIBHEIF_STATIC_BUILD
LIBS += $$DEPS/lib/libheif.a $$DEPS/lib/libde265.a -lpthread
QMAKE_LFLAGS += -Wl,--exclude-libs,ALL
# The SDK's CFLAGS give the static libraries debug info (60 MB of it).
QMAKE_POST_LINK += strip --strip-unneeded $(TARGET)

SOURCES += heifplugin.cpp
OTHER_FILES += heif.json build-deps.sh 10-procam-heif.rule

target.path = $$[QT_INSTALL_PLUGINS]/imageformats

# Tracker (localsearch) has no HEIF rule: without one the photos are not
# indexed as pictures and the Gallery does not list them.
tracker_rule.files = 10-procam-heif.rule
tracker_rule.path = /usr/share/localsearch3/extract-rules

# Both libraries' LGPL-3.0 texts are named COPYING.
licenses.extra = install -D -m 644 $$PWD/../third_party/libheif/COPYING $(INSTALL_ROOT)/usr/share/procam/licenses/libheif-COPYING && \
                 install -D -m 644 $$PWD/../third_party/libde265/COPYING $(INSTALL_ROOT)/usr/share/procam/licenses/libde265-COPYING
licenses.path = /usr/share/procam/licenses

INSTALLS += target tracker_rule licenses
