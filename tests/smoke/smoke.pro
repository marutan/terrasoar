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

include($$PWD/../tests.pri)

TARGET = tst_smoke
SOURCES += tst_smoke.cpp

# Path of the built application.
# app.pro sets DESTDIR so the executable is always in <build>/app.
win32 {
    TS_APP_EXE = $$shadowed($$TS_ROOT/app)/terrasoar.exe
} else {
    TS_APP_EXE = $$shadowed($$TS_ROOT/app)/terrasoar
}
DEFINES += TS_APP_PATH=\\\"$$TS_APP_EXE\\\"
