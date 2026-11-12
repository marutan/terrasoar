# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unit tests for ts/regions: region geometry to tile sets (antimeridian,
# poles, polygons), tile counts, the region list, pinned tiles in the disk
# cache, the bulk-download flag, and downloads and size estimates against
# a local mock WMS. No internet needed.

include($$PWD/../tests.pri)

TARGET = tst_regions
SOURCES += tst_regions.cpp
