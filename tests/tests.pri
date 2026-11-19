# SPDX-License-Identifier: GPL-2.0-or-later
#
# Terra Soar - A virtual globe
#
# Copyright (C) 2026 Peter Howkins
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.
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
    $$PWD/support/mockwms.h \
    $$PWD/support/nationalgriddata.h \
    $$PWD/support/offscreengl.h

# Absolute path of tests/data, for sample files.
DEFINES += TS_TEST_DATA_DIR=\\\"$$PWD/data\\\"
# Absolute path of the repository's data directory (the WMS catalogue).
DEFINES += TS_REPO_DATA_DIR=\\\"$$TS_ROOT/data\\\"

# Tests run from their build directory; `make check` uses
# TESTRUNNER/TESTARGS from the environment if set.
