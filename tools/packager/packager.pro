# SPDX-License-Identifier: GPL-2.0-or-later
#
# terrasoar-packager: builds the base pack from downloaded sources at build
# time (`make basepack` at the top level). See docs/tools/packager.md.

include($$PWD/../../common.pri)
include($$PWD/../../core/core.pri)
include($$PWD/packager.pri)

TEMPLATE = app
TARGET = terrasoar-packager
CONFIG += console
CONFIG -= app_bundle
QT -= widgets

SOURCES += $$PWD/src/main.cpp

OTHER_FILES += \
    basepack.json \
    build_basepack.py \
    fetch_sources.py \
    notices/*
