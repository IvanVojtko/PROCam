# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = app
TARGET = sfos-raw16-to-ppm

QT -= core gui
CONFIG += console
QMAKE_STRIP = :

QMAKE_CFLAGS += -std=c11 -Wall -Wextra

SOURCES += raw16-to-ppm.c

LIBS += -lm

target.path = /usr/libexec/procam
INSTALLS += target
