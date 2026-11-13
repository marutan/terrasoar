# SPDX-License-Identifier: GPL-2.0-or-later
#
# Third-party dependencies: GDAL, PROJ, GEOS (C API), SQLite and GeographicLib.
#
# Linux / macOS: found with pkg-config (packages gdal, proj, geos, sqlite3,
#   geographiclib).
#   Set PKG_CONFIG_PATH (and PKG_CONFIG_SYSROOT_DIR) to use a private prefix.
#
# Windows: point DEPS_ROOT at a prefix with include/, lib/ and bin/, either as
#   an environment variable or on the qmake command line:
#     qmake DEPS_ROOT=C:/deps terrasoar.pro
#   Import library names can be overridden with GDAL_LIB, PROJ_LIB, GEOS_LIB,
#   SQLITE_LIB and GEOGRAPHICLIB_LIB (defaults: gdal, proj, geos_c, sqlite3,
#   and GeographicLib-i if that import library exists, as in vcpkg's DLL
#   build, otherwise GeographicLib).
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
    # GeographicLib's CMake names a shared build's import library
    # GeographicLib-i.lib; a static build is GeographicLib.lib.
    isEmpty(GEOGRAPHICLIB_LIB) {
        exists($$DEPS_ROOT/lib/GeographicLib-i.lib) {
            GEOGRAPHICLIB_LIB = GeographicLib-i
        } else {
            GEOGRAPHICLIB_LIB = GeographicLib
        }
    }

    INCLUDEPATH += $$DEPS_ROOT/include
    LIBS += -L$$DEPS_ROOT/lib \
        -l$$GDAL_LIB -l$$PROJ_LIB -l$$GEOS_LIB -l$$SQLITE_LIB \
        -l$$GEOGRAPHICLIB_LIB
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += gdal proj geos sqlite3 geographiclib
    # pkg-config's include directories come with -I; also pass them as
    # -isystem (which wins for a directory given both ways) so warnings and
    # analysis cover only our own code, as for Qt's headers in common.pri.
    TS_DEPS_INCLUDES = $$system(pkg-config --cflags-only-I gdal proj geos sqlite3 geographiclib)
    for (flag, TS_DEPS_INCLUDES) {
        TS_DEPS_DIR = $$replace(flag, ^-I, )
        # Not the compiler's own directories (see common.pri).
        !contains(TS_SYSTEM_INCLUDE_DIRS, $$TS_DEPS_DIR): \
            QMAKE_CXXFLAGS += -isystem $$shell_quote($$TS_DEPS_DIR)
    }
}
