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
# Unit tests for ts/wmts: capabilities documents (tests/data/wmts), tile
# matrix maths, request building, and the WMTS imagery provider against a
# local mock server. No internet needed.

include($$PWD/../tests.pri)

TARGET = tst_wmts
SOURCES += tst_wmts.cpp

# The repository, for the catalogue in data/.
DEFINES += TS_SOURCE_DIR=\\\"$$TS_ROOT\\\"
