# SPDX-FileCopyrightText: 2026 ProCam Contributors
# SPDX-License-Identifier: BSD-3-Clause
TEMPLATE = app
TARGET = tst_scenescan
QT = core gui
CONFIG += console c++14
CONFIG -= app_bundle
SOURCES += tst_scenescan.cpp ../src/scenescan.cpp ../src/quirc.c
HEADERS += ../src/scenescan.h ../src/previewanalysis.h
