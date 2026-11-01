TEMPLATE = app
TARGET = tst_rawpipeline
QT += core gui testlib
CONFIG += testcase console c++14 object_parallel_to_source
# As the app (src/src.pro), so the tests cover the shipped code generation.
QMAKE_CXXFLAGS_RELEASE += -O3 -fno-math-errno
SOURCES += tst_rawpipeline.cpp ../src/rawpipelinemath.cpp ../src/rawpipeline.cpp \
        ../src/dnglensshading.cpp ../src/imageadjustments.cpp ../src/raisr.cpp
HEADERS += ../src/rawpipelinemath.h ../src/rawpipeline.h ../src/dnglensshading.h \
        ../src/imageadjustments.h ../src/raisr.h
