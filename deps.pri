# SPDX-License-Identifier: GPL-2.0-or-later
#
# Third-party dependencies: GDAL, PROJ, GEOS (C API) and SQLite.
#
# Linux / macOS: found with pkg-config (packages gdal, proj, geos, sqlite3).
#   Set PKG_CONFIG_PATH (and PKG_CONFIG_SYSROOT_DIR) to use a private prefix.
#
# Windows: point DEPS_ROOT at a prefix with include/, lib/ and bin/, either as
#   an environment variable or on the qmake command line:
#     qmake DEPS_ROOT=C:/deps terrasoar.pro
#   Import library names can be overridden with GDAL_LIB, PROJ_LIB, GEOS_LIB
#   and SQLITE_LIB (defaults: gdal, proj, geos_c, sqlite3).
#
# Projects that link the dependencies include this file (core.pri does it for
# everything that links tscore).

isEmpty(DEPS_ROOT): DEPS_ROOT = $$(DEPS_ROOT)

win32 {
    isEmpty(DEPS_ROOT) {
        error("DEPS_ROOT is not set. See docs/building.md.")
    }
    !exists($$DEPS_ROOT/include/gdal.h) {
        error("gdal.h not found under DEPS_ROOT=$$DEPS_ROOT/include.")
    }

    isEmpty(GDAL_LIB): GDAL_LIB = gdal
    isEmpty(PROJ_LIB): PROJ_LIB = proj
    isEmpty(GEOS_LIB): GEOS_LIB = geos_c
    isEmpty(SQLITE_LIB): SQLITE_LIB = sqlite3

    INCLUDEPATH += $$DEPS_ROOT/include
    LIBS += -L$$DEPS_ROOT/lib \
        -l$$GDAL_LIB -l$$PROJ_LIB -l$$GEOS_LIB -l$$SQLITE_LIB
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += gdal proj geos sqlite3
}
