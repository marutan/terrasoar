# Network and cache

How Terra Soar talks HTTP and keeps what it downloads: the network client
and its threads, the life of a request, HTTP freshness, and the tile disk
cache. Code: `core/include/ts/network/`. What WMS does with them is in
[wms.md](wms.md).

## Threads

```
loader threads (per source)         network thread                  GUI thread
  NetworkClient::fetch() ──────────► queue ─► QNetworkAccessManager   AddWmsDialog
    waits, polls "cancelled"           ≤ 6 on the wire                  start() → ticket,
  ◄──────────── response ────────────  retries, timeouts                polled by a timer
```

`ts::NetworkClient` owns one thread running a `QNetworkAccessManager`
(which must live on a thread with an event loop). The application has one
client, created with the first WMS layer and shared by every provider
through the `ProviderRegistry`; a program without WMS layers has no
network thread.

Requests come from two kinds of caller:

- **Loader threads** call `fetch()`, which blocks until the response
  arrives. It looks at the tile's cancellation flag every 20 ms; when the
  tile leaves the view it cancels the request and returns at once.
- **The GUI** calls `start()`, which returns a `NetworkTicket` at once; a
  timer polls `isFinished()` and the GUI never blocks.

Every hand-over between threads goes through a `std::mutex` (the queue,
each ticket's result) and the network thread is woken with a queued call
that carries no data: `drain()` invoked by name, so that not even a
functor is written on one thread and read on the other. ThreadSanitizer
sees `std::mutex` but not Qt's event queue, so data handed over through
queued calls would be reported as races (the reason for this design;
`tests/network` runs under TSan in CI). Qt's own HTTP thread hands replies
to ours through that queue too; TSan runs with
`ignore_noninstrumented_modules=1` so that what Qt does there isn't
reported (see `tests/tsan.supp`).

## The life of a request

1. **Queued.** At most `maximumInFlight` (6) requests hold a slot; the rest
   wait in order. The limit is the roadmap's "network requests in flight"
   budget, and what servers expect from one client.
2. **On the wire.** User-Agent `TerraSoar/<version>`; no HTTP cache in
   Qt (the disk cache does that); redirects followed unless from https to
   http.
3. **Answered.** Any HTTP status is a response (`NetworkError::None`); the
   caller decides what a 404 means. A body over `maximumResponseBytes`
   (16 MB) aborts the transfer (`TooLarge`).
4. **Retried** when worth it: a timeout, a connection failure, HTTP 408,
   429 or 5xx. Up to three attempts, 0.5 s then 1 s apart (doubling),
   longer if the server sends `Retry-After`, never more than 8 s. The
   request keeps its slot while it waits.
5. **Timed out** when nothing arrives for 15 s (Qt's transfer timeout,
   which resets with every byte, so a slow large download isn't cut off).
6. **Cancelled** at any point: a queued request is dropped, one on the
   wire has its connection aborted (the server sees the client go), one
   waiting to retry is finished. The caller gets `Cancelled`.

When the client is destroyed, everything open finishes as cancelled.

**Proxies.** By default the system's proxy settings apply
(`QNetworkProxyFactory::systemProxyForQuery()`: the `http_proxy` /
`https_proxy` / `no_proxy` environment variables on Linux, the system
settings on Windows). `NetworkSettings::proxy` sets one explicitly; the
tests use `QNetworkProxy::NoProxy`, and one test sends requests through a
mock proxy.

## HTTP freshness

`ts::httpFreshness()` decides how long a response may be used without
asking the server again, following RFC 9111 for a private cache:

| Response says | Freshness |
| --- | --- |
| `Cache-Control: no-store` | Not stored |
| `Cache-Control: no-cache` | Stored, stale at once (revalidated every time) |
| `Cache-Control: max-age=N` | N seconds, less `Age` |
| `Expires` | Until then, measured against the server's `Date` (clocks differ); an invalid date means stale |
| `Last-Modified` only | A tenth of the time since then, at most the default |
| Nothing | The default, 7 days |

Some map servers send none of these, hence the default, which matches
what others send (terrestris answers GetMap with `max-age=604800`, seven
days). A stale tile is
revalidated with `If-None-Match` (its ETag) or `If-Modified-Since`; a 304
renews its freshness from the 304's headers, keeping the old validators
if it sends none (`revalidatedFreshness()`). `parseHttpDate()` reads all
three HTTP date formats.

Stale tiles are shown as they are when the server can't be reached, and
when it answers with an error: an old map is more useful than none.

## The tile disk cache

`ts::TileDiskCache` keeps one source's tiles in one SQLite file, named
after a hash of the source's id (`wmsCacheFileName()`), in the cache
directory (`QStandardPaths::CacheLocation/wms`, for example
`~/.cache/Terra Soar/terrasoar/wms` on Linux).

```sql
CREATE TABLE tiles (
  tile          INTEGER PRIMARY KEY,  -- TileKey::packed()
  data          BLOB NOT NULL,        -- the response body as it came
  content_type  TEXT NOT NULL,
  expires       INTEGER NOT NULL,     -- fresh until (seconds since 1970)
  etag          TEXT NOT NULL,
  last_modified TEXT NOT NULL,
  last_used     INTEGER NOT NULL,     -- a counter, for LRU
  bytes         INTEGER NOT NULL);
CREATE INDEX tiles_last_used ON tiles(last_used);
PRAGMA user_version = 1;              -- the cache's format
```

- **Encoded, not decoded.** Tiles are stored as they came (PNG or JPEG),
  a tenth of their decoded size. Decoded images are kept in memory by the
  loader's LRU cache ([tiling.md](tiling.md)); the disk cache sits outside
  the memory budget.
- **Size limit and eviction.** 256 MB per source. The total is kept in
  memory; when a store passes the limit, the least recently used tiles are
  deleted, in batches, until it is under nine tenths of it (so not every
  store evicts). "Recently used" is a counter stored with each tile, so
  the order survives a restart and doesn't depend on the clock.
- **Opened lazily**, on the first tile, on a loader thread: nothing blocks
  the GUI, and a program without WMS layers opens nothing.
- **Write-ahead log**, `synchronous=NORMAL`: a crash can lose the last
  tiles, never the database.
- **One connection, one mutex.** Loader threads share the cache; each
  operation takes a mutex. A lookup, with its use recorded, takes about
  75 µs on the development machine (`benchmarkCacheFind`), against tens or
  hundreds of milliseconds for a request.

**Damage.** A cache is only a cache: if SQLite reports the file corrupt or
not a database, when opening it or later in use, the file (and its WAL)
is deleted and an empty cache starts in its place (`recoveries()` counts
how often). A file of another format version starts over the same way.
The user never sees an error from the cache.

## Tests

`tests/network`, against a local mock HTTP server
(`tests/support/mockhttpserver.h`, a `QTcpServer` on its own thread so a
test may block in a request to it; it can delay, stall, hang up, send too
much, redirect or act as a proxy):

- Responses, request headers, HTTP errors as answers, redirects.
- Retries: doubling pauses, Retry-After honoured and capped, giving up;
  connection refused and hang-ups; timeouts; oversized bodies.
- Cancellation drops the connection (the server sees the client go) and
  `fetch()` returns at once; tickets polled and cancelled; queued
  requests cancelled without being sent.
- The in-flight limit (12 requests, never more than 3 at the server);
  requests through a proxy; destruction cancels what is open; many
  threads sharing a client (TSan).
- HTTP dates in all three formats; freshness for every rule above.
- The disk cache: store, find, replace, refresh, remove; LRU eviction at
  the limit; the order of use surviving a restart; recovery from a
  garbage file, a truncated one, overwritten pages and an empty file; a
  file of another version; many threads sharing it; a lookup benchmark.
