# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = app
TARGET = sfos-camera2-probe

QT -= core gui
CONFIG += console
QMAKE_STRIP = :

QMAKE_CFLAGS += -std=c11 -Wall -Wextra

SOURCES += main.c

LIBS += -ldl

target.path = /usr/libexec/procam
INSTALLS += target
