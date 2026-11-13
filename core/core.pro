# SPDX-License-Identifier: GPL-2.0-or-later
#
# tscore: the Qt-widget-free core library (static).

include($$PWD/../common.pri)
include($$PWD/../deps.pri)

TEMPLATE = lib
TARGET = tscore
CONFIG += staticlib
CONFIG -= app_bundle

QT = core gui network $$TS_QT_OPENGL

INCLUDEPATH += $$PWD/include

HEADERS += \
    include/ts/base/buildinfo.h \
    include/ts/camera/camera.h \
    include/ts/camera/cameracontroller.h \
    include/ts/camera/scalebar.h \
    include/ts/deps/dependencies.h \
    include/ts/deps/gdaldataset.h \
    include/ts/deps/geoscontext.h \
    include/ts/deps/projcontext.h \
    include/ts/deps/sqliteconnection.h \
    include/ts/elevation/geoidmodel.h \
    include/ts/elevation/heightgrid.h \
    include/ts/elevation/heighttileloader.h \
    include/ts/elevation/sqliteelevationprovider.h \
    include/ts/geo/ellipsoid.h \
    include/ts/geo/geodesic.h \
    include/ts/geo/geodetic.h \
    include/ts/geo/geoformat.h \
    include/ts/geo/wgs84.h \
    include/ts/gl/glprobe.h \
    include/ts/globe/globerenderer.h \
    include/ts/globe/layerstackdraws.h \
    include/ts/globe/tilerenderer.h \
    include/ts/globe/vectorrenderer.h \
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
    include/ts/network/httpcache.h \
    include/ts/network/networkclient.h \
    include/ts/network/tilediskcache.h \
    include/ts/packs/basepack.h \
    include/ts/packs/packindex.h \
    include/ts/packs/packinstall.h \
    include/ts/packs/packlibrary.h \
    include/ts/packs/packmanifest.h \
    include/ts/perf/processstats.h \
    include/ts/projection/cylindricalprojection.h \
    include/ts/projection/projection.h \
    include/ts/providers/geopackagefeatureprovider.h \
    include/ts/providers/providerregistry.h \
    include/ts/providers/sqlitetileimageryprovider.h \
    include/ts/providers/testproviders.h \
    include/ts/providers/tileprovider.h \
    include/ts/regions/offlineregionstore.h \
    include/ts/regions/regiondownload.h \
    include/ts/regions/countryareas.h \
    include/ts/regions/regiongeometry.h \
    include/ts/render/framepacer.h \
    include/ts/render/glrenderdevice.h \
    include/ts/render/renderdevice.h \
    include/ts/tiling/lrucache.h \
    include/ts/tiling/tilekey.h \
    include/ts/tiling/tileloader.h \
    include/ts/tiling/tilemesh.h \
    include/ts/tiling/tileselector.h \
    include/ts/vector/vectormesh.h \
    include/ts/vector/vectorstyle.h \
    include/ts/vector/vectortileloader.h \
    include/ts/wms/wmscapabilities.h \
    include/ts/wms/wmscatalogue.h \
    include/ts/wms/wmsimageryprovider.h \
    include/ts/wms/wmssource.h \
    src/globe/globeuniforms.h \
    src/providers/sqlitestatementpool.h

SOURCES += \
    src/base/buildinfo.cpp \
    src/camera/camera.cpp \
    src/camera/cameracontroller.cpp \
    src/camera/scalebar.cpp \
    src/deps/dependencies.cpp \
    src/deps/gdaldataset.cpp \
    src/deps/geoscontext.cpp \
    src/deps/projcontext.cpp \
    src/deps/sqliteconnection.cpp \
    src/elevation/geoidmodel.cpp \
    src/elevation/heightgrid.cpp \
    src/elevation/heighttileloader.cpp \
    src/elevation/sqliteelevationprovider.cpp \
    src/geo/ellipsoid.cpp \
    src/geo/geodesic.cpp \
    src/geo/geodetic.cpp \
    src/geo/geoformat.cpp \
    src/gl/glprobe.cpp \
    src/globe/globerenderer.cpp \
    src/globe/layerstackdraws.cpp \
    src/globe/tilerenderer.cpp \
    src/globe/vectorrenderer.cpp \
    src/i18n/pseudotranslator.cpp \
    src/i18n/translationloader.cpp \
    src/layers/layerdefinition.cpp \
    src/layers/layertreemodel.cpp \
    src/layers/projectfile.cpp \
    src/maths/mat4.cpp \
    src/perf/frametimestats.cpp \
    src/network/httpcache.cpp \
    src/network/networkclient.cpp \
    src/network/tilediskcache.cpp \
    src/packs/basepack.cpp \
    src/packs/packindex.cpp \
    src/packs/packinstall.cpp \
    src/packs/packlibrary.cpp \
    src/packs/packmanifest.cpp \
    src/perf/processstats.cpp \
    src/projection/cylindricalprojection.cpp \
    src/projection/projection.cpp \
    src/providers/geopackagefeatureprovider.cpp \
    src/providers/providerregistry.cpp \
    src/providers/sqlitestatementpool.cpp \
    src/providers/sqlitetileimageryprovider.cpp \
    src/providers/testproviders.cpp \
    src/providers/tileprovider.cpp \
    src/regions/offlineregionstore.cpp \
    src/regions/regiondownload.cpp \
    src/regions/countryareas.cpp \
    src/regions/regiongeometry.cpp \
    src/render/framepacer.cpp \
    src/render/glrenderdevice.cpp \
    src/tiling/tilekey.cpp \
    src/tiling/tileloader.cpp \
    src/tiling/tilemesh.cpp \
    src/tiling/tileselector.cpp \
    src/vector/vectormesh.cpp \
    src/vector/vectorstyle.cpp \
    src/vector/vectortileloader.cpp \
    src/wms/wmscapabilities.cpp \
    src/wms/wmscatalogue.cpp \
    src/wms/wmsimageryprovider.cpp \
    src/wms/wmssource.cpp
