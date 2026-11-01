# SPDX-License-Identifier: BSD-3-Clause

TEMPLATE = app
TARGET = sfos-raw16-to-jpeg

QT -= core gui
CONFIG += console link_pkgconfig
QMAKE_STRIP = :
OBJECTS_DIR = .obj-jpeg

DEFINES += RAW16_TO_JPEG
QMAKE_CFLAGS += -std=c11 -Wall -Wextra
PKGCONFIG += libjpeg

SOURCES += raw16-to-jpeg.c

LIBS += -lm

target.path = /usr/libexec/procam
INSTALLS += target
