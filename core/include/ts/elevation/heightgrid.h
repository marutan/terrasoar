// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Height grids: how the base pack stores them, and sampling them.
 */
#ifndef TS_ELEVATION_HEIGHTGRID_H
#define TS_ELEVATION_HEIGHTGRID_H

#include <optional>
#include <vector>

#include <QByteArray>

#include "ts/providers/tileprovider.h"

namespace ts
{

/**
 * Returns @p heightM as the height grid encoding keeps it: to the nearest
 * 2 m above −100 m, to the nearest 10 m below (a global DEM's sea floor is
 * known to tens of metres at best, its land to metres).
 */
double quantizedHeightM(double heightM);

/**
 * Encodes @p samples² heights (row by row, row 0 north) for a tile store:
 * quantized as quantizedHeightM(), each value predicted from its west,
 * north and north-west neighbours, the residuals written as zig-zag
 * varints and compressed with zlib. About 4.8 bits a sample for ETOPO
 * 2022 at 5 km. See docs/design/terrain.md.
 */
QByteArray encodeHeightGrid(const std::vector<float>& heightsM, int samples);

/**
 * Decodes encodeHeightGrid()'s bytes into @p samples² heights, or returns
 * nothing if they are damaged or of another size.
 */
std::optional<std::vector<float>> decodeHeightGrid(const QByteArray& data,
                                                   int samples);

/**
 * Returns the height at (@p latDeg, @p lonDeg) in @p tile, interpolated
 * bilinearly, or nothing outside the tile.
 */
std::optional<double> sampleHeightM(const HeightTile& tile, double latDeg,
                                    double lonDeg);

/**
 * Returns the sea level at (@p latDeg, @p lonDeg) in @p tile
 * (HeightTile::seaLevelM), interpolated bilinearly; 0 where the tile has
 * none; nothing outside the tile.
 */
std::optional<double> sampleSeaLevelM(const HeightTile& tile, double latDeg,
                                      double lonDeg);

/**
 * Returns the heights of @p key, a descendant of @p ancestor's key, as
 * @p samples² samples interpolated bilinearly from @p ancestor (for keys
 * deeper than a source stores).
 */
HeightTile resampleHeightTile(const HeightTile& ancestor, TileKey key,
                              int samples);

/**
 * Returns the latitude and longitude of sample (@p row, @p column) of a
 * @p samples-wide grid over @p key (row 0 at the north edge).
 */
GeoPoint heightSamplePoint(TileKey key, int samples, int row, int column);

} // namespace ts

#endif // TS_ELEVATION_HEIGHTGRID_H
