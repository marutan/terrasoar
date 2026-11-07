# SPDX-License-Identifier: GPL-2.0-or-later

include($$PWD/../tests.pri)

TARGET = tst_mainwindow
SOURCES += tst_mainwindow.cpp

# Build the application shell's sources into the test.
include($$TS_ROOT/app/app.pri)
QT += widgets
