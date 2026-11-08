TEMPLATE = app
TARGET = tst_exifmetadata
QT += core gui testlib
CONFIG += testcase console c++14 link_pkgconfig
PKGCONFIG += libexif libtiff-4
SOURCES += exifmetadata.cpp ../src/exifutils.cpp ../src/heicencode.cpp
HEADERS += ../src/exifutils.h ../src/heicencode.h
