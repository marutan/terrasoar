# SPDX-License-Identifier: GPL-2.0-or-later
#
# Include this to link against tscore (adds include path, library and the
# third-party dependencies). Include common.pri before it.

TS_CORE_SRC = $$PWD
TS_CORE_OUT = $$shadowed($$PWD)

INCLUDEPATH += $$TS_CORE_SRC/include
DEPENDPATH += $$TS_CORE_SRC/include

win32 {
    CONFIG(debug, debug|release): TS_CORE_LIBDIR = $$TS_CORE_OUT/debug
    else: TS_CORE_LIBDIR = $$TS_CORE_OUT/release
    msvc: TS_CORE_LIBFILE = $$TS_CORE_LIBDIR/tscore.lib
    else: TS_CORE_LIBFILE = $$TS_CORE_LIBDIR/libtscore.a
} else {
    TS_CORE_LIBDIR = $$TS_CORE_OUT
    TS_CORE_LIBFILE = $$TS_CORE_LIBDIR/libtscore.a
}

LIBS = -L$$TS_CORE_LIBDIR -ltscore $$LIBS
PRE_TARGETDEPS += $$TS_CORE_LIBFILE

win32: LIBS += -lpsapi

include($$TS_ROOT/deps.pri)
