# SPDX-License-Identifier: GPL-2.0-or-later
#
# The application shell's sources, without main(). Shared by app.pro and the
# UI tests in tests/ui so both build the same code.

QT += widgets $$TS_QT_OPENGLWIDGETS

INCLUDEPATH += $$PWD/src

HEADERS += \
    $$PWD/src/globeview.h \
    $$PWD/src/mainwindow.h \
    $$PWD/src/startupmeasurement.h

SOURCES += \
    $$PWD/src/globeview.cpp \
    $$PWD/src/mainwindow.cpp \
    $$PWD/src/startupmeasurement.cpp

# Translations: compiled by lrelease during the build and embedded as
# :/i18n/terrasoar_<locale>.qm.
TRANSLATIONS = $$TS_ROOT/translations/terrasoar_en.ts
CONFIG += lrelease embed_translations
QM_FILES_RESOURCE_PREFIX = /i18n
