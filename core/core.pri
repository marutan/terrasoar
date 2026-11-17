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
# Include this to link against tscore (adds include path, library and the
# third-party dependencies). Include common.pri before it.

TS_CORE_SRC = $$PWD
TS_CORE_OUT = $$shadowed($$PWD)

# tscore is static, so its users link the Qt modules it uses.
QT += gui network $$TS_QT_OPENGL

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
