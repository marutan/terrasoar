# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unit tests for ts/elevation: the height grid encoding, sampling, geoid
# models and the elevation provider.

include($$PWD/../tests.pri)

QT += network

TARGET = tst_elevation
SOURCES += tst_elevation.cpp
