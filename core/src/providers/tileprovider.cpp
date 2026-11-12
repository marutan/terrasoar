// SPDX-License-Identifier: GPL-2.0-or-later
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
