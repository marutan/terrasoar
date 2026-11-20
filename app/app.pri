# SPDX-License-Identifier: GPL-2.0-or-later
#
# Terra Soar - A virtual globe
#
# Copyright (C) 2026 Peter Howkins
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.
#
# The application shell's sources, without main(). Shared by app.pro and the
# UI tests in tests/ui so both build the same code.

QT += widgets $$TS_QT_OPENGLWIDGETS

INCLUDEPATH += $$PWD/src

RESOURCES += $$PWD/app.qrc $$PWD/userguide.qrc

HEADERS += \
    $$PWD/src/aboutdialog.h \
    $$PWD/src/appsettings.h \
    $$PWD/src/helpbrowser.h \
    $$PWD/src/helpwindow.h \
    $$PWD/src/addwebterraindialog.h \
    $$PWD/src/projectionsettingsdialog.h \
    $$PWD/src/addwmsdialog.h \
    $$PWD/src/addwmtsdialog.h \
    $$PWD/src/addxyzdialog.h \
    $$PWD/src/elevationprofiledialog.h \
    $$PWD/src/featureinfowindow.h \
    $$PWD/src/globeview.h \
    $$PWD/src/layertreedock.h \
    $$PWD/src/mainwindow.h \
    $$PWD/src/newregiondialog.h \
    $$PWD/src/offlineregionmanager.h \
    $$PWD/src/packmanager.h \
    $$PWD/src/packmanagerdialog.h \
    $$PWD/src/offlineregionsdialog.h \
    $$PWD/src/startupmeasurement.h \
    $$PWD/src/timebar.h \
    $$PWD/src/timemodebox.h \
    $$PWD/src/userguide.h \
    $$PWD/src/wmscataloguefile.h

SOURCES += \
    $$PWD/src/aboutdialog.cpp \
    $$PWD/src/appsettings.cpp \
    $$PWD/src/helpbrowser.cpp \
    $$PWD/src/helpwindow.cpp \
    $$PWD/src/addwebterraindialog.cpp \
    $$PWD/src/projectionsettingsdialog.cpp \
    $$PWD/src/addwmsdialog.cpp \
    $$PWD/src/addwmtsdialog.cpp \
    $$PWD/src/addxyzdialog.cpp \
    $$PWD/src/elevationprofiledialog.cpp \
    $$PWD/src/featureinfowindow.cpp \
    $$PWD/src/globeview.cpp \
    $$PWD/src/layertreedock.cpp \
    $$PWD/src/mainwindow.cpp \
    $$PWD/src/newregiondialog.cpp \
    $$PWD/src/offlineregionmanager.cpp \
    $$PWD/src/packmanager.cpp \
    $$PWD/src/packmanagerdialog.cpp \
    $$PWD/src/offlineregionsdialog.cpp \
    $$PWD/src/startupmeasurement.cpp \
    $$PWD/src/timebar.cpp \
    $$PWD/src/timemodebox.cpp \
    $$PWD/src/userguide.cpp \
    $$PWD/src/wmscataloguefile.cpp

# Translations: compiled by lrelease during the build and embedded as
# :/i18n/terrasoar_<locale>.qm.
TRANSLATIONS = $$TS_ROOT/translations/terrasoar_en.ts
CONFIG += lrelease embed_translations
QM_FILES_RESOURCE_PREFIX = /i18n
