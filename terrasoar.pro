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
    .clang-tidy \
    Doxyfile \
    README.md \
    docs/*.md

# `make lupdate` refreshes translations/terrasoar_en.ts from the sources.
lupdate.commands = python3 $$shell_path($$PWD/tools/i18n/update_translations.py)
QMAKE_EXTRA_TARGETS += lupdate

# `make analyze` runs the static analysis CI runs (clang-tidy, clazy and
# cppcheck, Linux only), in its own Clang build under analyze/ here. Run
# tools/checks/run_static_analysis.py directly for --changed and other options.
analyze.commands = python3 $$shell_path($$PWD/tools/checks/run_static_analysis.py) \
    --build-dir $$shell_path($$OUT_PWD/analyze)
QMAKE_EXTRA_TARGETS += analyze

# `make basepack` downloads the base pack's sources (pinned by SHA-256) and
# builds the pack into basepack/ here, where the application finds it. See
# docs/tools/packager.md.
basepack.commands = python3 $$shell_path($$PWD/tools/packager/build_basepack.py) \
    --build-dir $$shell_path($$OUT_PWD)
basepack.depends = sub-tools
QMAKE_EXTRA_TARGETS += basepack

# `make packs` builds the extra data packs (tools/packager/packs/*.json) into
# packs/ here, with their release files and index entries. Nothing is
# uploaded. See docs/tools/packager.md.
packs.commands = python3 $$shell_path($$PWD/tools/packager/build_pack.py) \
    --build-dir $$shell_path($$OUT_PWD) \
    --config $$shell_path($$PWD/tools/packager/packs/bluemarble-full.json) && \
    python3 $$shell_path($$PWD/tools/packager/build_pack.py) \
    --build-dir $$shell_path($$OUT_PWD) \
    --config $$shell_path($$PWD/tools/packager/packs/shoreline-full.json)
packs.depends = sub-tools
QMAKE_EXTRA_TARGETS += packs

# `make docs` builds the Doxygen API documentation into build/docs/api.
docs.commands = cd $$shell_path($$PWD) && doxygen Doxyfile
QMAKE_EXTRA_TARGETS += docs
