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
# GPU tests for ts/globe: golden images and precision near the surface.

include($$PWD/../tests.pri)
# The VMAP0 golden images build their packs from tests/data/vmap0.
include($$TS_ROOT/tools/packager/packager.pri)

TARGET = tst_globe
SOURCES += tst_globe.cpp
DEFINES += TS_ROOT_DIR=\\\"$$TS_ROOT\\\"
