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
    include/ts/elevation/compositeelevationprovider.h \
    include/ts/elevation/elevationprofile.h \
    include/ts/elevation/gdalelevationprovider.h \
    include/ts/elevation/geoidmodel.h \
    include/ts/elevation/heightgrid.h \
    include/ts/elevation/heighttileloader.h \
    include/ts/elevation/sqliteelevationprovider.h \
    include/ts/elevation/webterrainprovider.h \
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
    include/ts/kml/kmlactivity.h \
    include/ts/kml/kmldocument.h \
    include/ts/kml/kmledit.h \
    include/ts/kml/kmlfeatureprovider.h \
    include/ts/kml/kmloverlays.h \
    include/ts/kml/kmlpresentation.h \
    include/ts/kml/kmlreader.h \
    include/ts/kml/kmlwriter.h \
    include/ts/layers/layerdefinition.h \
    include/ts/layers/layertreemodel.h \
    include/ts/layers/projectfile.h \
    include/ts/maths/angles.h \
    include/ts/maths/mat4.h \
    include/ts/maths/ray.h \
    include/ts/maths/splitdouble.h \
    include/ts/maths/vec3.h \
    include/ts/maths/vec4.h \
    include/ts/orbits/earthframes.h \
    include/ts/orbits/elementsource.h \
    include/ts/orbits/orbitalelements.h \
    include/ts/orbits/orbitcomputer.h \
    include/ts/orbits/satellitedraws.h \
    include/ts/orbits/satellitelayer.h \
    include/ts/orbits/satellitepasses.h \
    include/ts/orbits/satellitetrack.h \
    include/ts/orbits/sgp4.h \
    include/ts/perf/frametimestats.h \
    include/ts/network/cachedtilefetcher.h \
    include/ts/network/httpcache.h \
    include/ts/network/networkclient.h \
    include/ts/network/regiontilesource.h \
    include/ts/network/tilediskcache.h \
    include/ts/packs/basepack.h \
    include/ts/packs/packindex.h \
    include/ts/packs/packinstall.h \
    include/ts/packs/packlibrary.h \
    include/ts/packs/packmanifest.h \
    include/ts/perf/processstats.h \
    include/ts/projection/arc.h \
    include/ts/projection/azimuthal.h \
    include/ts/projection/conic.h \
    include/ts/projection/cylindricalprojection.h \
    include/ts/projection/datumshift.h \
    include/ts/projection/localexpansion.h \
    include/ts/projection/nationalgrid.h \
    include/ts/projection/normalcylindrical.h \
    include/ts/projection/rotatedmercator.h \
    include/ts/projection/transversemercator.h \
    include/ts/projection/worldmaps.h \
    include/ts/projection/projection.h \
    include/ts/providers/gdalimageryprovider.h \
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
    include/ts/vector/featurepick.h \
    include/ts/vector/stylerules.h \
    include/ts/vector/vectormesh.h \
    include/ts/vector/vectorstyle.h \
    include/ts/vector/vectortileloader.h \
    include/ts/wms/wmscapabilities.h \
    include/ts/wms/wmscatalogue.h \
    include/ts/wms/wmsfeatureinfo.h \
    include/ts/wms/wmsimageryprovider.h \
    include/ts/wms/wmssource.h \
    include/ts/time/layertime.h \
    include/ts/time/programclock.h \
    include/ts/time/timeextent.h \
    include/ts/wfs/wfscapabilities.h \
    include/ts/wfs/wfsfeatureprovider.h \
    include/ts/wfs/wfssource.h \
    include/ts/wmts/wmtscapabilities.h \
    include/ts/wmts/wmtscrs.h \
    include/ts/wmts/wmtsimageryprovider.h \
    include/ts/wmts/wmtssource.h \
    include/ts/xyz/xyzimageryprovider.h \
    include/ts/xyz/xyzsource.h \
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
    src/elevation/compositeelevationprovider.cpp \
    src/elevation/elevationprofile.cpp \
    src/elevation/gdalelevationprovider.cpp \
    src/elevation/geoidmodel.cpp \
    src/elevation/heightgrid.cpp \
    src/elevation/heighttileloader.cpp \
    src/elevation/sqliteelevationprovider.cpp \
    src/elevation/webterrainprovider.cpp \
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
    src/kml/kmlactivity.cpp \
    src/kml/kmldocument.cpp \
    src/kml/kmledit.cpp \
    src/kml/kmlfeatureprovider.cpp \
    src/kml/kmloverlays.cpp \
    src/kml/kmlpresentation.cpp \
    src/kml/kmlreader.cpp \
    src/kml/kmlwriter.cpp \
    src/layers/layerdefinition.cpp \
    src/layers/layertreemodel.cpp \
    src/layers/projectfile.cpp \
    src/maths/mat4.cpp \
    src/orbits/earthframes.cpp \
    src/orbits/elementsource.cpp \
    src/orbits/orbitalelements.cpp \
    src/orbits/orbitcomputer.cpp \
    src/orbits/satellitedraws.cpp \
    src/orbits/satellitelayer.cpp \
    src/orbits/satellitepasses.cpp \
    src/orbits/satellitetrack.cpp \
    src/orbits/sgp4.cpp \
    src/perf/frametimestats.cpp \
    src/network/cachedtilefetcher.cpp \
    src/network/httpcache.cpp \
    src/network/networkclient.cpp \
    src/network/regiontilesource.cpp \
    src/network/tilediskcache.cpp \
    src/packs/basepack.cpp \
    src/packs/packindex.cpp \
    src/packs/packinstall.cpp \
    src/packs/packlibrary.cpp \
    src/packs/packmanifest.cpp \
    src/perf/processstats.cpp \
    src/projection/arc.cpp \
    src/projection/azimuthal.cpp \
    src/projection/conic.cpp \
    src/projection/ellipsoidal.h \
    src/projection/cylindricalprojection.cpp \
    src/projection/datumshift.cpp \
    src/projection/localexpansion.cpp \
    src/projection/nationalgrid.cpp \
    src/projection/normalcylindrical.cpp \
    src/projection/rotatedmercator.cpp \
    src/projection/transversemercator.cpp \
    src/projection/worldmaps.cpp \
    src/projection/projection.cpp \
    src/providers/gdalimageryprovider.cpp \
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
    src/vector/featurepick.cpp \
    src/vector/stylerules.cpp \
    src/vector/vectormesh.cpp \
    src/vector/vectorstyle.cpp \
    src/vector/vectortileloader.cpp \
    src/wms/wmscapabilities.cpp \
    src/wms/wmscatalogue.cpp \
    src/wms/wmsfeatureinfo.cpp \
    src/wms/wmsimageryprovider.cpp \
    src/wms/wmssource.cpp \
    src/time/layertime.cpp \
    src/time/programclock.cpp \
    src/time/timeextent.cpp \
    src/wfs/wfscapabilities.cpp \
    src/wfs/wfsfeatureprovider.cpp \
    src/wfs/wfssource.cpp \
    src/wmts/wmtscapabilities.cpp \
    src/wmts/wmtscrs.cpp \
    src/wmts/wmtsimageryprovider.cpp \
    src/wmts/wmtssource.cpp \
    src/xyz/xyzimageryprovider.cpp \
    src/xyz/xyzsource.cpp
