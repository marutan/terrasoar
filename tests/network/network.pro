# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unit tests for ts/network: the HTTP client against a local mock server,
# HTTP freshness rules, the tile disk cache. No internet needed.

include($$PWD/../tests.pri)

TARGET = tst_network
SOURCES += tst_network.cpp
