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
# Regression and unit tests. `make check` runs them all.
# Each core subsection has its own project here (see docs/testing.md);
# tests/template/ is the starting point for a new one and is not built.

TEMPLATE = subdirs

SUBDIRS = \
    base \
    deps \
    perf \
    i18n \
    gl \
    ui \
    maths \
    geo \
    render \
    camera \
    globe \
    tiling \
    providers \
    layers \
    packs \
    packager \
    vector \
    projection \
    network \
    wms \
    wmts \
    regions \
    elevation \
    help \
    xyz \
    time \
    wfs \
    kml \
    orbits

# The smoke tests launch the application with QProcess, whose vfork-based
# start-up the ThreadSanitizer runtime cannot follow (it aborts with a CHECK
# failure). TSan builds cover the unit and stress tests; the smoke tests run
# in every other configuration.
!tsan: SUBDIRS += smoke
