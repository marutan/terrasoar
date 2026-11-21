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
# The packager's code without its main(), shared by the tool and
# tests/packager. Include common.pri and core/core.pri before it.

INCLUDEPATH += $$PWD/src

HEADERS += \
    $$PWD/src/countrybuilder.h \
    $$PWD/src/elevationbuilder.h \
    $$PWD/src/imagerytiler.h \
    $$PWD/src/packbuilder.h \
    $$PWD/src/shorelinebuilder.h \
    $$PWD/src/vmap0builder.h \
    $$PWD/src/vpf/vpfcoverage.h \
    $$PWD/src/vpf/vpftable.h

SOURCES += \
    $$PWD/src/countrybuilder.cpp \
    $$PWD/src/elevationbuilder.cpp \
    $$PWD/src/imagerytiler.cpp \
    $$PWD/src/packbuilder.cpp \
    $$PWD/src/shorelinebuilder.cpp \
    $$PWD/src/vmap0builder.cpp \
    $$PWD/src/vpf/vpfcoverage.cpp \
    $$PWD/src/vpf/vpftable.cpp
