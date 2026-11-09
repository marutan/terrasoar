# SPDX-License-Identifier: GPL-2.0-or-later
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
    packs

# The smoke tests launch the application with QProcess, whose vfork-based
# start-up the ThreadSanitizer runtime cannot follow (it aborts with a CHECK
# failure). TSan builds cover the unit and stress tests; the smoke tests run
# in every other configuration.
!tsan: SUBDIRS += smoke
