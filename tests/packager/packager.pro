# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unit tests for the base-pack packager (tools/packager), on generated
# fixtures, plus checks of a built pack when TS_BASEPACK_DIR names one.

include($$PWD/../tests.pri)
include($$TS_ROOT/tools/packager/packager.pri)

TARGET = tst_packager
SOURCES += tst_packager.cpp
