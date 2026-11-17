// SPDX-License-Identifier: GPL-2.0-or-later
/*
  Terra Soar - A virtual globe

  Copyright (C) 2026 Peter Howkins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "ts/providers/tileprovider.h"

#include <utility>

namespace ts
{

TileProvider::~TileProvider() = default;

ProviderStatus TileProvider::status() const
{
  const std::lock_guard<std::mutex> lock(m_statusMutex);
  return m_status;
}

void TileProvider::setStatusCallback(std::function<void()> callback)
{
  const std::lock_guard<std::mutex> lock(m_statusMutex);
  m_statusCallback = std::move(callback);
}

void TileProvider::reportStatus(const ProviderStatus& status)
{
  std::function<void()> callback;
  {
    const std::lock_guard<std::mutex> lock(m_statusMutex);
    if (m_status == status) {
      return;
    }
    m_status = status;
    callback = m_statusCallback;
  }
  if (callback) {
    callback();
  }
}

ImageLoadResult
ImageryProvider::loadTileImage(TileKey key, const std::atomic<bool>& cancelled)
{
  ImageLoadResult result;
  result.image = loadImage(key, cancelled);
  if (cancelled.load()) {
    result.outcome = ImageLoadResult::Outcome::Cancelled;
  } else {
    result.outcome = result.image.isNull() ? ImageLoadResult::Outcome::NoData
                                           : ImageLoadResult::Outcome::Image;
  }
  return result;
}

} // namespace ts
