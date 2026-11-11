# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unit tests for ts/wms: capabilities documents (tests/data/wms), request
# building and axis order, the catalogue, and the WMS imagery provider
# against a local mock server. No internet needed.

include($$PWD/../tests.pri)

TARGET = tst_wms
SOURCES += tst_wms.cpp
