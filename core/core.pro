# SPDX-License-Identifier: GPL-2.0-or-later
#
# tscore: the Qt-widget-free core library (static).

include($$PWD/../common.pri)
include($$PWD/../deps.pri)

TEMPLATE = lib
TARGET = tscore
CONFIG += staticlib
CONFIG -= app_bundle

QT = core gui $$TS_QT_OPENGL

INCLUDEPATH += $$PWD/include

HEADERS += \
    include/ts/base/buildinfo.h \
    include/ts/camera/camera.h \
    include/ts/camera/cameracontroller.h \
    include/ts/deps/dependencies.h \
    include/ts/deps/gdaldataset.h \
    include/ts/deps/geoscontext.h \
    include/ts/deps/projcontext.h \
    include/ts/deps/sqliteconnection.h \
    include/ts/geo/ellipsoid.h \
    include/ts/geo/geodesic.h \
    include/ts/geo/geodetic.h \
    include/ts/geo/geoformat.h \
    include/ts/geo/wgs84.h \
    include/ts/gl/glprobe.h \
    include/ts/globe/globerenderer.h \
    include/ts/globe/layerstackdraws.h \
    include/ts/globe/tilerenderer.h \
    include/ts/i18n/pseudotranslator.h \
    include/ts/i18n/translationloader.h \
    include/ts/layers/layerdefinition.h \
    include/ts/layers/layertreemodel.h \
    include/ts/layers/projectfile.h \
    include/ts/maths/angles.h \
    include/ts/maths/mat4.h \
    include/ts/maths/ray.h \
    include/ts/maths/splitdouble.h \
    include/ts/maths/vec3.h \
    include/ts/maths/vec4.h \
    include/ts/perf/frametimestats.h \
    include/ts/packs/basepack.h \
    include/ts/packs/packmanifest.h \
    include/ts/perf/processstats.h \
    include/ts/providers/providerregistry.h \
    include/ts/providers/testproviders.h \
    include/ts/providers/tileprovider.h \
    include/ts/render/framepacer.h \
    include/ts/render/glrenderdevice.h \
    include/ts/render/renderdevice.h \
    include/ts/tiling/lrucache.h \
    include/ts/tiling/tilekey.h \
    include/ts/tiling/tileloader.h \
    include/ts/tiling/tilemesh.h \
    include/ts/tiling/tileselector.h \
    src/globe/globeuniforms.h

SOURCES += \
    src/base/buildinfo.cpp \
    src/camera/camera.cpp \
    src/camera/cameracontroller.cpp \
    src/deps/dependencies.cpp \
    src/deps/gdaldataset.cpp \
    src/deps/geoscontext.cpp \
    src/deps/projcontext.cpp \
    src/deps/sqliteconnection.cpp \
    src/geo/ellipsoid.cpp \
    src/geo/geodesic.cpp \
    src/geo/geodetic.cpp \
    src/geo/geoformat.cpp \
    src/gl/glprobe.cpp \
    src/globe/globerenderer.cpp \
    src/globe/layerstackdraws.cpp \
    src/globe/tilerenderer.cpp \
    src/i18n/pseudotranslator.cpp \
    src/i18n/translationloader.cpp \
    src/layers/layerdefinition.cpp \
    src/layers/layertreemodel.cpp \
    src/layers/projectfile.cpp \
    src/maths/mat4.cpp \
    src/perf/frametimestats.cpp \
    src/packs/basepack.cpp \
    src/packs/packmanifest.cpp \
    src/perf/processstats.cpp \
    src/providers/providerregistry.cpp \
    src/providers/testproviders.cpp \
    src/providers/tileprovider.cpp \
    src/render/framepacer.cpp \
    src/render/glrenderdevice.cpp \
    src/tiling/tilekey.cpp \
    src/tiling/tileloader.cpp \
    src/tiling/tilemesh.cpp \
    src/tiling/tileselector.cpp
