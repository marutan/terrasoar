// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/heightgrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ts
{

namespace
{

/** First version of the encoding; the first byte of every tile. */
constexpr char formatVersion = 1;

/** The height grid's integer code of @p heightM (see quantizedHeightM()). */
std::int32_t heightCode(double heightM)
{
  if (heightM >= -100.0) {
    return static_cast<std::int32_t>(std::lround(heightM / 2.0));
  }
  return -50 + static_cast<std::int32_t>(std::lround((heightM + 100.0) / 10.0));
}

double codeHeightM(std::int32_t code)
{
  return code >= -50 ? 2.0 * code : -100.0 + 10.0 * (code + 50);
}

/** Index of sample (@p row, @p column) in a @p samples-wide grid. */
std::size_t indexOf(int row, int column, int samples)
{
  return static_cast<std::size_t>(row) * static_cast<std::size_t>(samples) +
         static_cast<std::size_t>(column);
}

/** The prediction of value (@p row, @p column) from its decoded neighbours. */
// Row then column, as everywhere in a height grid.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::int32_t predicted(const std::vector<std::int32_t>& codes, int samples,
                       int row, int column)
{
  const std::size_t n = static_cast<std::size_t>(samples);
  const std::size_t r = static_cast<std::size_t>(row);
  const std::size_t c = static_cast<std::size_t>(column);
  if (row == 0 && column == 0) {
    return 0;
  }
  if (row == 0) {
    return codes[c - 1];
  }
  if (column == 0) {
    return codes[(r - 1) * n];
  }
  return codes[r * n + c - 1] + codes[(r - 1) * n + c] -
         codes[(r - 1) * n + c - 1];
}

double heightAt(const HeightTile& tile, int row, int column)
{
  return static_cast<double>(tile.heightsM[indexOf(row, column, tile.samples)]);
}

} // namespace

double quantizedHeightM(double heightM)
{
  return codeHeightM(heightCode(heightM));
}

QByteArray encodeHeightGrid(const std::vector<float>& heightsM, int samples)
{
  const std::size_t count =
      static_cast<std::size_t>(samples) * static_cast<std::size_t>(samples);
  if (samples < 2 || heightsM.size() != count) {
    return QByteArray();
  }
  std::vector<std::int32_t> codes(count);
  for (std::size_t i = 0; i < count; ++i) {
    codes[i] = heightCode(static_cast<double>(heightsM[i]));
  }
  QByteArray residuals;
  residuals.reserve(static_cast<int>(count * 2));
  for (int row = 0; row < samples; ++row) {
    for (int column = 0; column < samples; ++column) {
      const std::int32_t value = codes[indexOf(row, column, samples)];
      const std::int32_t residual =
          value - predicted(codes, samples, row, column);
      // Zig-zag: small magnitudes of either sign become small numbers.
      std::uint32_t zigzag = (static_cast<std::uint32_t>(residual) << 1U) ^
                             (residual < 0 ? 0xffffffffU : 0U);
      while (zigzag >= 0x80U) {
        residuals.append(static_cast<char>((zigzag & 0x7fU) | 0x80U));
        zigzag >>= 7U;
      }
      residuals.append(static_cast<char>(zigzag));
    }
  }
  QByteArray data(1, formatVersion);
  data.append(qCompress(residuals, 9));
  return data;
}

std::optional<std::vector<float>> decodeHeightGrid(const QByteArray& data,
                                                   int samples)
{
  if (samples < 2 || data.size() < 2 || data.at(0) != formatVersion) {
    return std::nullopt;
  }
  const QByteArray residuals = qUncompress(data.mid(1));
  const std::size_t count =
      static_cast<std::size_t>(samples) * static_cast<std::size_t>(samples);
  std::vector<std::int32_t> codes(count);
  std::size_t position = 0;
  const std::size_t size = static_cast<std::size_t>(residuals.size());
  for (int row = 0; row < samples; ++row) {
    for (int column = 0; column < samples; ++column) {
      std::uint32_t zigzag = 0;
      for (unsigned shift = 0;; shift += 7U) {
        if (position >= size || shift > 28U) {
          return std::nullopt;
        }
        const unsigned char byte = static_cast<unsigned char>(
            residuals.at(static_cast<int>(position++)));
        zigzag |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
          break;
        }
      }
      const std::int32_t residual = static_cast<std::int32_t>(zigzag >> 1U) ^
                                    -static_cast<std::int32_t>(zigzag & 1U);
      codes[indexOf(row, column, samples)] =
          predicted(codes, samples, row, column) + residual;
    }
  }
  if (position != size) {
    return std::nullopt;
  }
  std::vector<float> heights(count);
  for (std::size_t i = 0; i < count; ++i) {
    heights[i] = static_cast<float>(codeHeightM(codes[i]));
  }
  return heights;
}

std::optional<double> sampleHeightM(const HeightTile& tile, double latDeg,
                                    double lonDeg)
{
  const GeoRect bounds = tile.key.bounds();
  const int n = tile.samples;
  if (n < 2 ||
      tile.heightsM.size() !=
          static_cast<std::size_t>(n) * static_cast<std::size_t>(n) ||
      latDeg < bounds.southDeg || latDeg > bounds.northDeg ||
      lonDeg < bounds.westDeg || lonDeg > bounds.eastDeg) {
    return std::nullopt;
  }
  // Grid coordinates: row 0 north, column 0 west.
  const double y = (bounds.northDeg - latDeg) / bounds.heightDeg() * (n - 1);
  const double x = (lonDeg - bounds.westDeg) / bounds.widthDeg() * (n - 1);
  const int row = std::clamp(static_cast<int>(std::floor(y)), 0, n - 2);
  const int column = std::clamp(static_cast<int>(std::floor(x)), 0, n - 2);
  const double fy = y - row;
  const double fx = x - column;
  const double north = heightAt(tile, row, column) * (1.0 - fx) +
                       heightAt(tile, row, column + 1) * fx;
  const double south = heightAt(tile, row + 1, column) * (1.0 - fx) +
                       heightAt(tile, row + 1, column + 1) * fx;
  return north * (1.0 - fy) + south * fy;
}

// Size, then row and column, as everywhere in a height grid.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
GeoPoint heightSamplePoint(TileKey key, int samples, int row, int column)
{
  const GeoRect bounds = key.bounds();
  const double step = 1.0 / static_cast<double>(samples - 1);
  return GeoPoint{bounds.northDeg - bounds.heightDeg() * row * step,
                  bounds.westDeg + bounds.widthDeg() * column * step};
}

HeightTile resampleHeightTile(const HeightTile& ancestor, TileKey key,
                              int samples)
{
  HeightTile tile;
  tile.key = key;
  tile.samples = samples;
  tile.heightsM.resize(static_cast<std::size_t>(samples) *
                       static_cast<std::size_t>(samples));
  for (int row = 0; row < samples; ++row) {
    for (int column = 0; column < samples; ++column) {
      const GeoPoint point = heightSamplePoint(key, samples, row, column);
      tile.heightsM[indexOf(row, column, samples)] = static_cast<float>(
          sampleHeightM(ancestor, point.latDeg, point.lonDeg).value_or(0.0));
    }
  }
  return tile;
}

} // namespace ts
