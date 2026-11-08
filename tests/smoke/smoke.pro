# SPDX-License-Identifier: GPL-2.0-or-later

include($$PWD/../tests.pri)

TARGET = tst_smoke
SOURCES += tst_smoke.cpp

# Path of the built application.
# app.pro sets DESTDIR so the executable is always in <build>/app.
win32 {
    TS_APP_EXE = $$shadowed($$TS_ROOT/app)/terrasoar.exe
} else {
    TS_APP_EXE = $$shadowed($$TS_ROOT/app)/terrasoar
}
DEFINES += TS_APP_PATH=\\\"$$TS_APP_EXE\\\"
