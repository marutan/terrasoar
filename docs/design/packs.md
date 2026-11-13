# Data packs

How extra data packs are listed, downloaded, checked, installed, updated
and removed, and how an installed pack takes over from the base pack.
Code: `core/include/ts/packs/`, the window in `app/src/packmanager*.{h,cpp}`.
The packs themselves are built by the packager
([docs/tools/packager.md](../tools/packager.md)); the base pack is
described in [docs/data/base-pack.md](../data/base-pack.md). For users:
[offline-use.md](../user/offline-use.md#extra-data-packs).

```
Data Packs window (GUI thread)            PackLibrary  (<data>/packs/)
  index ← GET packs.json (PackIndex)        <id>/            installed packs
  Install / Update / Remove                 .staging/<id>/   downloads in progress
  progress polled 4× a second               .trash/          left for the next start
    │
    └─ PackInstall (one thread per pack)
         GET manifest → check SHA-256
         per file: Range GETs of 8 MB → <file>.part → check SHA-256
         all files good → PackLibrary::activate()
```

## What a pack is

A pack is a directory with a `manifest.json` (the format of
`ts::PackManifest`, see [base-pack.md](../data/base-pack.md#file-formats))
and the files it lists. The base pack is one; extra packs have the same
layout. A dataset of an extra pack may say which base pack dataset it
**replaces**:

```json
{"id": "bluemarble", "kind": "imagery", "file": "bluemarble.sqlite",
 "replaces": "bluemarble", …}
```

While such a pack is installed, layer sources `{"kind": "basepack",
"dataset": "bluemarble"}` read its file instead of the base pack's, and
show its attribution. Projects therefore need no change, and removing the
pack brings the base data back. A replacing dataset holds all the levels
of the one it replaces, so only one file is read per dataset.
`ts::ProviderRegistry::setInstalledPacks()` makes the replacements;
providers are keyed by the file they read, so layers pick up the new data
on the next `refreshLayers()`. If two installed packs replace the same
dataset, the one whose id sorts first wins (there is one per dataset so
far).

`"replaces"` is optional, so the manifest format stays at 1: older
versions ignore the field.

## The first packs

| Pack id | Replaces | What | Size |
| --- | --- | --- | --- |
| `bluemarble-full` | `bluemarble` | The same Blue Marble image as the base pack (21600 × 10800 px), tiled to level 6 instead of 4: about 1.2 km per pixel instead of 4.9 km | 67 MB |
| `shoreline-full` | `shoreline` | GSHHG 2.3.7 with the high and full resolutions: crude to intermediate as in the base pack, high (0.2 km) from level 6, full (10.9 million vertices) from level 8 | 241 MB |
| `geoid-egm2008-fine` | `egm2008` | EGM2008 at its published 2.5′ grid (PROJ's `us_nga_egm08_25.tif`) instead of the base pack's 5′, as 16-bit centimetres: within 1 cm of PROJ's grid instead of 0.4 m | 13.4 MB |
| `geoid-osgm15` | (a datum of its own, `osgm15`) | Ordnance Survey's OSGM15 for Ordnance Datum Newlyn (PROJ's `uk_os_OSGM15_GB.tif`, shipped as it is, BSD-2-Clause) | 1.5 MB |

A geoid pack replacing a base pack geoid makes every elevation source on
that datum use the finer grid (the registry keys providers by the grid in
use, so the terrain is reloaded when the pack is installed or removed).
One that brings a datum of its own adds it to the datums elevation files
and web terrain can be measured from (`ProviderRegistry::verticalDatums()`,
see [vertical-datums.md](vertical-datums.md)).

Their packager configurations are `tools/packager/packs/*.json`; `make packs`
builds them with their release files and index entries
([docs/tools/packager.md](../tools/packager.md#extra-packs)).

## The index

The application reads one URL, `ts::defaultPackIndexUrl()`: `index.json`
on the main branch of the `marutan/terrasoar-packs` repository. The
environment variable `TS_PACK_INDEX_URL` overrides it (tests, mirrors).
Each entry carries absolute URLs, so the files can live anywhere; they are
attached to that repository's releases, one release per pack version.

```json
{
  "format": 1,
  "packs": [
    {
      "id": "bluemarble-full",
      "version": "2004-07.1",
      "name": "Blue Marble, full resolution",
      "description": "…",
      "licence": "Public domain (NASA)",
      "bytes": 104857600,
      "manifest": {"url": "https://…/manifest.json",
                   "sha256": "…", "bytes": 2048},
      "files": {"bluemarble.sqlite": "https://…/bluemarble.sqlite",
                "licences/…": "https://…"}
    }
  ]
}
```

`ts::PackIndex::fromJson()` refuses a newer format and skips (with a
log line) an entry that is malformed, so one bad entry doesn't hide the
rest. An entry must give a URL for every file its manifest lists; that is
checked when the manifest arrives. The index's SHA-256 of the manifest,
and the manifest's SHA-256 of every file, chain the files to the index:
whoever can change the index can change the pack, so it is fetched over
HTTPS only (a test can allow `http://127.0.0.1`).

**Versions** are opaque strings: a pack is "out of date" when the
installed version differs from the index's. Downgrades therefore also
show as updates, which is what an index that withdrew a bad version
wants.

## Downloading

`ts::PackInstall` runs on its own thread with the shared
`NetworkClient` (so its proxy, retries and limits apply). It works through
the files the manifest lists, in order:

1. A file already in the staging directory with the listed size and
   SHA-256 is kept: an install that stopped after it doesn't fetch it
   again.
2. Otherwise it appends to `<file>.part`. Each request asks for the next
   8 MB with `Range: bytes=<have>-<have+8 MB−1>`. A `206` whose
   `Content-Range` starts at `<have>` is appended. A `200` is the whole
   file from a server that ignores ranges: it replaces the part file if it
   is the listed size (larger files can't come whole within the client's
   16 MB response limit, and then the install fails with a message saying
   the server doesn't support resuming). A `416` means the part file is
   already complete or wrong: it is checked as it is.
3. At the listed size the part file's SHA-256 is computed. If it matches
   the file is renamed into place; if not, the part file is deleted and
   the install fails ("… is damaged (checksum mismatch)"). Nothing
   damaged is ever activated.

Progress is bytes on disk over the pack's total, polled by the window
four times a second, as for regions. **Pause** stops the thread after the
current request and keeps the part files; **Resume** starts a new
`PackInstall`, which continues from them. **Cancel** stops it and deletes
the staging directory. Quitting is a pause: the next start lists the pack
as "Paused" (the staging directory has a `pending.json` naming the index
entry), and Resume continues where it stopped. A network failure after
the client's retries also pauses, with the error shown, so the user can
resume later.

## Installing, updating and removing

`ts::PackLibrary` owns `<AppLocalDataLocation>/packs/` (on Linux
`~/.local/share/Terra Soar/terrasoar/packs`; the environment variable
`TS_PACKS_DIR` replaces it, for tests and portable installs):

- **Activate** (all files good): the manifest is written into the
  staging directory last, then the installed `<id>/` (if any, an update)
  is renamed into `.trash/`, the staging directory to `<id>/`, and the
  trash deleted. Before activating, the window tells the registry to
  forget the pack and refreshes the layers, so on Windows no file of the
  old version is open; if a rename still fails, the staging directory is
  kept with `ready` in its `pending.json`, and the next start finishes the
  swap before anything is opened.
- **Remove**: forget the pack, refresh the layers, rename `<id>/` into
  `.trash/` and delete it. Whatever can't be deleted now (Windows) stays in
  `.trash/`, which every start empties first. The pack is gone from the
  list either way, and no file of it is left once the trash is emptied.
- **On start-up**: empty `.trash/`, finish `ready` swaps, then read each
  `<id>/manifest.json` and check every file's size (not its SHA-256: that
  was checked on install, and reading 300 MB at every start would be
  slow). A pack with a missing or wrongly sized file is listed as
  "Damaged" and not used; Install fetches it again.

## Tests

`tests/packs` against the mock HTTP server: index parsing (bad entries
skipped, newer format refused); an install whose server hangs up part way
resumes with a `Range` request from the bytes on disk; a server ignoring
ranges works for a small file and fails clearly for a large one; a bad
checksum is rejected and leaves nothing installed; an update replaces the
old version; remove leaves no file behind; a `ready` swap is finished by
the next start; the registry reads a replacing dataset while its pack is
installed and the base pack's after it is removed. `tests/ui` drives the
Data Packs window against the mock server.
