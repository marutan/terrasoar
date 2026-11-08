# SPDX-License-Identifier: GPL-2.0-or-later
#
# Settings shared by every Terra Soar project file. Include it first:
#
#   include($$PWD/../common.pri)

TS_ROOT = $$PWD

# --- Qt version switch -------------------------------------------------------

lessThan(QT_MAJOR_VERSION, 5) | \
  if(equals(QT_MAJOR_VERSION, 5):lessThan(QT_MINOR_VERSION, 15)) {
    error("Terra Soar needs Qt 5.15 or Qt 6 (found $$[QT_VERSION]).")
}

# Name of the module that provides QOpenGLWidget. Projects that use it add
# QT += $$TS_QT_OPENGLWIDGETS.
greaterThan(QT_MAJOR_VERSION, 5) {
    TS_QT_OPENGLWIDGETS = openglwidgets
} else {
    TS_QT_OPENGLWIDGETS = widgets
}

# --- Language and warnings -------------------------------------------------

CONFIG += c++17 warn_on
CONFIG -= c++11 c++14
# qmake on Qt 5 does not always translate c++17 for MSVC.
msvc: QMAKE_CXXFLAGS += /std:c++17 /permissive- /Zc:__cplusplus /utf-8

# Every API deprecated up to and including Qt 5.15 is a compile error, so the
# code builds unchanged on Qt 5 and Qt 6.
DEFINES += \
    QT_DISABLE_DEPRECATED_BEFORE=0x060000 \
    QT_NO_CAST_FROM_ASCII \
    QT_NO_CAST_TO_ASCII \
    QT_NO_URL_CAST_FROM_STRING \
    QT_NO_NARROWING_CONVERSIONS_IN_CONNECT \
    QT_USE_QSTRINGBUILDER

# The full warning set, on every build (CONFIG+=werror makes them errors).
# qmake's warn_on already adds -Wall -Wextra on GCC and Clang; they are listed
# here too so the policy doesn't depend on the mkspec. See
# docs/coding-standards.md for why each flag is on.
gcc|clang {
    QMAKE_CXXFLAGS_WARN_ON += \
        -Wall -Wextra -Wpedantic \
        -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual \
        -Wconversion -Wsign-conversion -Wold-style-cast \
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
}
# Clang only: GCC's -Wnull-dereference reports false positives in inlined
# libstdc++ code at -O2 (e.g. inside std::nth_element).
clang: QMAKE_CXXFLAGS_WARN_ON += -Wnull-dereference
# qmake passes Qt's headers with -I, so the compiler checks them with our
# warning flags; Clang's -Wpedantic then rejects Qt 6.8's [[nodiscard("…")]]
# under C++17. Marking Qt's include directories as system headers keeps
# warnings for our own code only (for a directory given both ways, -isystem
# wins).
gcc|clang {
    TS_QT_HEADERS = $$[QT_INSTALL_HEADERS]
    QMAKE_CXXFLAGS += -isystem $$shell_quote($$TS_QT_HEADERS)
    for (module, $$list(QtCore QtGui QtWidgets QtOpenGL QtOpenGLWidgets \
                        QtTest QtDBus)) {
        exists($$TS_QT_HEADERS/$$module): \
            QMAKE_CXXFLAGS += -isystem $$shell_quote($$TS_QT_HEADERS/$$module)
    }
}

# Qt 6's QSKIP/QFAIL-style macros are variadic and are used with no extra
# arguments; Clang's -Wpedantic flags that in Qt's macro, not our code.
clang: QMAKE_CXXFLAGS_WARN_ON += -Wno-gnu-zero-variadic-macro-arguments
# Load only the shared libraries we call into. GDAL pulls in over a hundred
# indirect dependencies; Ubuntu's GCC already links --as-needed, Clang and
# other distributions do not. Keeps start-up time and memory down.
linux: QMAKE_LFLAGS += -Wl,--as-needed

msvc {
    # Replace the mkspec's -W3 with /W4 (otherwise cl warns D9025 on every
    # file). As with -isystem above, headers included with <...> (Qt, vcpkg
    # libraries) are external and get no warnings, so /WX checks only our own
    # code; project headers are included with "..." and keep /W4. Qt 5.15's
    # qstringbuilder.h otherwise fails with C4127.
    QMAKE_CXXFLAGS_WARN_ON -= -W3
    QMAKE_CXXFLAGS_WARN_ON += /W4 /external:anglebrackets /external:W0
    DEFINES += NOMINMAX _USE_MATH_DEFINES
}

# CONFIG+=werror turns warnings into errors (used by CI).
werror {
    gcc|clang: QMAKE_CXXFLAGS_WARN_ON += -Werror
    msvc: QMAKE_CXXFLAGS_WARN_ON += /WX
}

# --- Optional instrumented builds ---------------------------------------------

# CONFIG+=msvc_analyze: MSVC's code analysis (/analyze) of our own code;
# headers included with <...> are external and not analysed. With
# CONFIG+=werror its warnings are errors too. Used by the nightly CI.
msvc_analyze {
    !msvc: error("CONFIG+=msvc_analyze needs MSVC.")
    QMAKE_CXXFLAGS += /analyze /analyze:external-
}

# CONFIG+=compiledb (Clang): every object also writes a compile-database
# fragment, <object>.json, which tools/checks/run_static_analysis.py merges
# into compile_commands.json for clang-tidy and clazy. `$@` is make's
# target, so the fragment sits next to its object file.
compiledb {
    !clang: error("CONFIG+=compiledb needs Clang (qmake -spec linux-clang).")
    QMAKE_CXXFLAGS += -MJ $@.json
}

# CONFIG+=tsan: ThreadSanitizer build (Linux, GCC or Clang).
tsan {
    QMAKE_CXXFLAGS += -fsanitize=thread -fno-omit-frame-pointer -g -O1
    QMAKE_LFLAGS += -fsanitize=thread
    DEFINES += TS_TSAN_BUILD
}

# CONFIG+=asan: AddressSanitizer + UBSan build. Undefined behaviour stops
# the program (-fno-sanitize-recover), so a test that hits it fails.
asan {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined \
        -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

# CONFIG+=coverage: gcov/llvm-cov instrumentation.
coverage {
    QMAKE_CXXFLAGS += --coverage -O0 -g
    QMAKE_LFLAGS += --coverage
}

# --- Version -----------------------------------------------------------------

TS_VERSION = 0.0.1
DEFINES += TS_VERSION_STRING=\\\"$$TS_VERSION\\\"
