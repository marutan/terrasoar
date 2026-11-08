# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build-time tools. Compiled tools (e.g. the base-pack packager in Phase 3)
# are added to SUBDIRS; scripts under checks/, i18n/ and perf/ need Python 3
# and are not built.

TEMPLATE = subdirs
SUBDIRS =

OTHER_FILES += \
    checks/*.py \
    i18n/*.py \
    perf/*.py \
    new_test_project.py
