# SPDX-License-Identifier: GPL-2.0-or-later
#
# Terra Soar top-level project.
#
#   qmake terrasoar.pro && make && make check
#
# See docs/building.md for per-OS instructions.

TEMPLATE = subdirs

SUBDIRS = core app tools tests

app.depends = core
tools.depends = core
tests.depends = core app

OTHER_FILES += \
    common.pri \
    deps.pri \
    .clang-format \
    Doxyfile \
    README.md \
    docs/*.md

# `make lupdate` refreshes translations/terrasoar_en.ts from the sources.
lupdate.commands = python3 $$shell_path($$PWD/tools/i18n/update_translations.py)
QMAKE_EXTRA_TARGETS += lupdate

# `make docs` builds the Doxygen API documentation into build/docs/api.
docs.commands = cd $$shell_path($$PWD) && doxygen Doxyfile
QMAKE_EXTRA_TARGETS += docs
