# SPDX-License-Identifier: GPL-2.0-or-later
#
# The application shell's sources, without main(). Shared by app.pro and the
# UI tests in tests/ui so both build the same code.

QT += widgets $$TS_QT_OPENGLWIDGETS

INCLUDEPATH += $$PWD/src

RESOURCES += $$PWD/app.qrc

HEADERS += \
    $$PWD/src/addwebterraindialog.h \
    $$PWD/src/addwmsdialog.h \
    $$PWD/src/globeview.h \
    $$PWD/src/layertreedock.h \
    $$PWD/src/mainwindow.h \
    $$PWD/src/newregiondialog.h \
    $$PWD/src/offlineregionmanager.h \
    $$PWD/src/packmanager.h \
    $$PWD/src/packmanagerdialog.h \
    $$PWD/src/offlineregionsdialog.h \
    $$PWD/src/startupmeasurement.h \
    $$PWD/src/wmscataloguefile.h

SOURCES += \
    $$PWD/src/addwebterraindialog.cpp \
    $$PWD/src/addwmsdialog.cpp \
    $$PWD/src/globeview.cpp \
    $$PWD/src/layertreedock.cpp \
    $$PWD/src/mainwindow.cpp \
    $$PWD/src/newregiondialog.cpp \
    $$PWD/src/offlineregionmanager.cpp \
    $$PWD/src/packmanager.cpp \
    $$PWD/src/packmanagerdialog.cpp \
    $$PWD/src/offlineregionsdialog.cpp \
    $$PWD/src/startupmeasurement.cpp \
    $$PWD/src/wmscataloguefile.cpp

# Translations: compiled by lrelease during the build and embedded as
# :/i18n/terrasoar_<locale>.qm.
TRANSLATIONS = $$TS_ROOT/translations/terrasoar_en.ts
CONFIG += lrelease embed_translations
QM_FILES_RESOURCE_PREFIX = /i18n
