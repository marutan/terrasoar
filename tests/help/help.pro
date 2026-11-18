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
#
# Unit and UI tests for the user guide built into the program
# (Help > User Guide): its pages, links, anchors, search and window.

include($$PWD/../tests.pri)

TARGET = tst_help
SOURCES += tst_help.cpp

# The guide's code is in the application shell; build its sources in.
include($$TS_ROOT/app/app.pri)
QT += widgets

# The source tree, to check that every page of docs/user/ is built in and
# that links out of the guide name files that exist.
DEFINES += TS_SOURCE_DIR=\\\"$$TS_ROOT\\\"
