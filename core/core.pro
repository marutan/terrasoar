# SPDX-License-Identifier: GPL-2.0-or-later
#
# tscore: the Qt-widget-free core library (static).

include($$PWD/../common.pri)
include($$PWD/../deps.pri)

TEMPLATE = lib
TARGET = tscore
CONFIG += staticlib
CONFIG -= app_bundle

QT = core gui

INCLUDEPATH += $$PWD/include

HEADERS += \
    include/ts/base/buildinfo.h \
    include/ts/deps/dependencies.h \
    include/ts/deps/gdaldataset.h \
    include/ts/deps/geoscontext.h \
    include/ts/deps/projcontext.h \
    include/ts/deps/sqliteconnection.h \
    include/ts/gl/glprobe.h \
    include/ts/i18n/pseudotranslator.h \
    include/ts/i18n/translationloader.h \
    include/ts/maths/angles.h \
    include/ts/maths/mat4.h \
    include/ts/maths/ray.h \
    include/ts/maths/vec3.h \
    include/ts/maths/vec4.h \
    include/ts/perf/frametimestats.h \
    include/ts/perf/processstats.h

SOURCES += \
    src/base/buildinfo.cpp \
    src/deps/dependencies.cpp \
    src/deps/gdaldataset.cpp \
    src/deps/geoscontext.cpp \
    src/deps/projcontext.cpp \
    src/deps/sqliteconnection.cpp \
    src/gl/glprobe.cpp \
    src/i18n/pseudotranslator.cpp \
    src/i18n/translationloader.cpp \
    src/maths/mat4.cpp \
    src/perf/frametimestats.cpp \
    src/perf/processstats.cpp
