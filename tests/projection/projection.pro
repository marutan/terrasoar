# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unit tests for ts/projection. The GLSL-against-C++ case needs OpenGL
# 3.3 and skips without it; the rest needs no display.

include($$PWD/../tests.pri)

TARGET = tst_projection
SOURCES += tst_projection.cpp
