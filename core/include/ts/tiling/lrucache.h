// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A thread-safe least-recently-used cache with a cost budget.
 */
#ifndef TS_TILING_LRUCACHE_H
#define TS_TILING_LRUCACHE_H

#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace ts
{

/**
 * Keeps values up to a total cost and drops the least recently used ones
 * when an insert would exceed it.
 *
 * Costs are whatever the caller measures (bytes for decoded tiles). A
 * value costing more than the whole budget is not kept. Values are
 * returned by copy, so use cheap-to-copy types (Qt's implicitly shared
 * QImage, std::shared_ptr<const T>).
 *
 * All member functions lock one mutex; the cache can be shared between
 * worker threads and the render thread.
 *
 * @threadsafe
 */
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class LruCache
{
public:
  /** Creates a cache holding at most @p maximumCost. */
  explicit LruCache(std::size_t maximumCost) : m_maximumCost(maximumCost) {}

  /**
   * Returns the value for @p key and marks it most recently used, or
   * nothing.
   */
  std::optional<Value> find(const Key& key)
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const typename Map::iterator it = m_map.find(key);
    if (it == m_map.end()) {
      ++m_misses;
      return std::nullopt;
    }
    ++m_hits;
    m_order.splice(m_order.begin(), m_order, it->second);
    return it->second->value;
  }

  /**
   * Inserts or replaces the value for @p key with cost @p cost, then drops
   * least recently used entries until the total fits the budget.
   */
  void insert(const Key& key, Value value, std::size_t cost)
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const typename Map::iterator existing = m_map.find(key);
    if (existing != m_map.end()) {
      m_cost -= existing->second->cost;
      m_order.erase(existing->second);
      m_map.erase(existing);
    }
    if (cost > m_maximumCost) {
      return;
    }
    m_order.push_front(Entry{key, std::move(value), cost});
    m_map.emplace(key, m_order.begin());
    m_cost += cost;
    evictLocked();
  }

  /** Removes @p key if present. */
  void remove(const Key& key)
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const typename Map::iterator it = m_map.find(key);
    if (it != m_map.end()) {
      m_cost -= it->second->cost;
      m_order.erase(it->second);
      m_map.erase(it);
    }
  }

  /** Removes everything. */
  void clear()
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_order.clear();
    m_map.clear();
    m_cost = 0;
  }

  /** Changes the budget, dropping entries if it shrank. */
  void setMaximumCost(std::size_t maximumCost)
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_maximumCost = maximumCost;
    evictLocked();
  }

  /** Returns the budget. */
  std::size_t maximumCost() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_maximumCost;
  }

  /** Returns the total cost held. */
  std::size_t totalCost() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_cost;
  }

  /** Returns the number of entries. */
  std::size_t size() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_map.size();
  }

  /** Returns how many find() calls found their key. */
  std::size_t hits() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_hits;
  }

  /** Returns how many find() calls did not. */
  std::size_t misses() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_misses;
  }

private:
  struct Entry {
    Key key;
    Value value;
    std::size_t cost;
  };
  using List = std::list<Entry>;
  using Map = std::unordered_map<Key, typename List::iterator, Hash>;

  void evictLocked()
  {
    while (m_cost > m_maximumCost && !m_order.empty()) {
      const Entry& last = m_order.back();
      m_cost -= last.cost;
      m_map.erase(last.key);
      m_order.pop_back();
    }
  }

  mutable std::mutex m_mutex;
  List m_order;
  Map m_map;
  std::size_t m_maximumCost;
  std::size_t m_cost = 0;
  std::size_t m_hits = 0;
  std::size_t m_misses = 0;
};

} // namespace ts

#endif // TS_TILING_LRUCACHE_H
