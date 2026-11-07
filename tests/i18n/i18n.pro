# SPDX-License-Identifier: GPL-2.0-or-later

include($$PWD/../tests.pri)

TARGET = tst_i18n
SOURCES += tst_i18n.cpp

# Embed the English .qm so the loader can be tested against it.
TRANSLATIONS = $$TS_ROOT/translations/terrasoar_en.ts
CONFIG += lrelease embed_translations
QM_FILES_RESOURCE_PREFIX = /i18n
