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

gcc|clang {
    QMAKE_CXXFLAGS_WARN_ON += -Wpedantic -Wshadow -Wnon-virtual-dtor
}
# Qt 6's QSKIP/QFAIL-style macros are variadic and are used with no extra
# arguments; Clang's -Wpedantic flags that in Qt's macro, not our code.
clang: QMAKE_CXXFLAGS_WARN_ON += -Wno-gnu-zero-variadic-macro-arguments
# Load only the shared libraries we call into. GDAL pulls in over a hundred
# indirect dependencies; Ubuntu's GCC already links --as-needed, Clang and
# other distributions do not. Keeps start-up time and memory down.
linux: QMAKE_LFLAGS += -Wl,--as-needed

msvc {
    QMAKE_CXXFLAGS_WARN_ON += /W4
    DEFINES += NOMINMAX _USE_MATH_DEFINES
}

# CONFIG+=werror turns warnings into errors (used by CI).
werror {
    gcc|clang: QMAKE_CXXFLAGS_WARN_ON += -Werror
    msvc: QMAKE_CXXFLAGS_WARN_ON += /WX
}

# --- Optional instrumented builds ---------------------------------------------

# CONFIG+=tsan: ThreadSanitizer build (Linux, GCC or Clang).
tsan {
    QMAKE_CXXFLAGS += -fsanitize=thread -fno-omit-frame-pointer -g -O1
    QMAKE_LFLAGS += -fsanitize=thread
    DEFINES += TS_TSAN_BUILD
}

# CONFIG+=asan: AddressSanitizer + UBSan build.
asan {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
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
