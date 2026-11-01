# Host tool: qmake tools/raisr/raisr.pro && make
TEMPLATE = app
TARGET = raisr-train
QT += core gui
CONFIG += console c++14 object_parallel_to_source
QMAKE_CXXFLAGS += -O2
SOURCES += raisr-train.cpp ../../src/raisr.cpp
HEADERS += ../../src/raisr.h ../../src/rawpipelinemath.h
