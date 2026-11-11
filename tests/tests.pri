# SPDX-License-Identifier: GPL-2.0-or-later
#
# Common settings for test projects. A test project is:
#
#   include($$PWD/../tests.pri)
#   TARGET = tst_<name>
#   SOURCES += tst_<name>.cpp

include($$PWD/../common.pri)
include($$TS_ROOT/core/core.pri)

TEMPLATE = app
QT += testlib

CONFIG += testcase console
CONFIG -= app_bundle

# Test helpers shared by several test projects.
INCLUDEPATH += $$PWD/support
HEADERS += \
    $$PWD/support/concurrency.h \
    $$PWD/support/fixturepack.h \
    $$PWD/support/goldenimage.h \
    $$PWD/support/mockhttpserver.h \
    $$PWD/support/offscreengl.h

# Absolute path of tests/data, for sample files.
DEFINES += TS_TEST_DATA_DIR=\\\"$$PWD/data\\\"

# Tests run from their build directory; `make check` uses
# TESTRUNNER/TESTARGS from the environment if set.
