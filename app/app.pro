# SPDX-License-Identifier: GPL-2.0-or-later
#
# terrasoar: the desktop application (UI shell over tscore).

include($$PWD/../common.pri)
include($$PWD/../core/core.pri)
include($$PWD/app.pri)

TEMPLATE = app
TARGET = terrasoar
CONFIG -= app_bundle
QT += core gui

# Keep the executable at <build>/app/terrasoar on every platform so tests
# and scripts can find it.
DESTDIR = $$OUT_PWD

SOURCES += src/main.cpp

isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/bin
INSTALLS += target
