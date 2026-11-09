# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build-time tools: the base-pack packager (packager/, compiled); scripts
# under checks/, i18n/, perf/ and packager/ need Python 3 and are not built.

TEMPLATE = subdirs
SUBDIRS = packager

OTHER_FILES += \
    checks/*.py \
    i18n/*.py \
    perf/*.py \
    new_test_project.py
