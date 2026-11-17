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
# terrasoar-packager: builds the base pack from downloaded sources at build
# time (`make basepack` at the top level). See docs/tools/packager.md.

include($$PWD/../../common.pri)
include($$PWD/../../core/core.pri)
include($$PWD/packager.pri)

TEMPLATE = app
TARGET = terrasoar-packager
CONFIG += console
CONFIG -= app_bundle
QT -= widgets

SOURCES += $$PWD/src/main.cpp

OTHER_FILES += \
    basepack.json \
    build_basepack.py \
    build_pack.py \
    fetch_sources.py \
    notices/* \
    packs/*.json
