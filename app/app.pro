# SPDX-License-Identifier: GPL-2.0-or-later
#
# terrasoar: the desktop application (UI shell over tscore).

include($$PWD/../common.pri)
include($$PWD/../core/core.pri)
include($$PWD/app.pri)

TEMPLATE = app
TARGET = terrasoar
CONFIG -= app_bundle
QT += core gui

# Keep the executable at <build>/app/terrasoar on every platform so tests
# and scripts can find it.
DESTDIR = $$OUT_PWD

SOURCES += src/main.cpp

# The Windows program's icon and version information (Explorer's
# Properties, the installer's list of programs).
win32 {
    RC_ICONS = $$PWD/../packaging/icons/terrasoar.ico
    VERSION = $${TS_VERSION}.0
    QMAKE_TARGET_COMPANY = "Peter Howkins"
    QMAKE_TARGET_PRODUCT = "Terra Soar"
    QMAKE_TARGET_DESCRIPTION = "Terra Soar"
    QMAKE_TARGET_COPYRIGHT = "Peter Howkins. GNU GPL v2 or later."
}

# `make install` (INSTALL_ROOT=<dir> to stage it) lays out a Linux install
# under PREFIX: bin/terrasoar, share/terrasoar/basepack (the application
# finds it there), the desktop entry, AppStream metadata, MIME type, icons
# and the licence notices. The packages are built from it
# (docs/release/packaging.md). The base pack is taken from BASEPACK_DIR,
# by default the one `make basepack` builds.
isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/bin
INSTALLS += target

unix:!macx {
    TS_APP_ID = io.github.marutan.TerraSoar
    TS_PACKAGING = $$PWD/../packaging

    desktop.files = $$TS_PACKAGING/linux/$${TS_APP_ID}.desktop
    desktop.path = $$PREFIX/share/applications
    metainfo.files = $$TS_PACKAGING/linux/$${TS_APP_ID}.metainfo.xml
    metainfo.path = $$PREFIX/share/metainfo
    mime.files = $$TS_PACKAGING/linux/$${TS_APP_ID}.xml
    mime.path = $$PREFIX/share/mime/packages
    notices.files = $$PWD/../THIRD_PARTY_NOTICES $$PWD/../COPYING \
        $$PWD/../CHANGELOG.md
    notices.path = $$PREFIX/share/doc/terrasoar
    INSTALLS += desktop metainfo mime notices

    # Icons under the application id, in the hicolor theme.
    icons.path = $$PREFIX/share/icons/hicolor
    icons.extra = $(MKDIR) $(INSTALL_ROOT)$$PREFIX/share/icons/hicolor/scalable/apps && \
        $(INSTALL_FILE) $$TS_PACKAGING/icons/terrasoar.svg \
        $(INSTALL_ROOT)$$PREFIX/share/icons/hicolor/scalable/apps/$${TS_APP_ID}.svg
    for (size, $$list(16 24 32 48 64 128 256 512)) {
        icons.extra += && $(MKDIR) $(INSTALL_ROOT)$$PREFIX/share/icons/hicolor/$${size}x$${size}/apps && \
            $(INSTALL_FILE) $$TS_PACKAGING/icons/png/terrasoar-$${size}.png \
            $(INSTALL_ROOT)$$PREFIX/share/icons/hicolor/$${size}x$${size}/apps/$${TS_APP_ID}.png
    }
    INSTALLS += icons

    isEmpty(BASEPACK_DIR): BASEPACK_DIR = $$OUT_PWD/../basepack
    exists($$BASEPACK_DIR/manifest.json) {
        basepack.files = $$BASEPACK_DIR/*
        basepack.path = $$PREFIX/share/terrasoar/basepack
        INSTALLS += basepack
    } else {
        message("No base pack in $$BASEPACK_DIR: make install leaves it out (make basepack, or set BASEPACK_DIR).")
    }
}
