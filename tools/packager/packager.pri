# SPDX-License-Identifier: GPL-2.0-or-later
#
# The packager's code without its main(), shared by the tool and
# tests/packager. Include common.pri and core/core.pri before it.

INCLUDEPATH += $$PWD/src

HEADERS += \
    $$PWD/src/imagerytiler.h \
    $$PWD/src/packbuilder.h \
    $$PWD/src/shorelinebuilder.h

SOURCES += \
    $$PWD/src/imagerytiler.cpp \
    $$PWD/src/packbuilder.cpp \
    $$PWD/src/shorelinebuilder.cpp
