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
| `bluemarble-monthly` | (variants of `bluemarble`, see [Variants](#variants)) | Blue Marble for each month of 2004 (5400 × 2700 px each), tiled to level 3: the seasons follow the program's clock | 24.9 MB |
| `geoid-osgm15` ("UK grids") | (a datum of its own, `osgm15`) | Ordnance Survey's OSGM15 for Ordnance Datum Newlyn (PROJ's `uk_os_OSGM15_GB.tif`, shipped as it is, BSD-2-Clause); from version 2015.2 also OSTN15 (PROJ's `uk_os_OSTN15_NTv2_OSGBtoETRS.tif`, BSD-2-Clause) for the UK National Grid projection | 4.5 MB |
| `vmap0-*` (17 packs) | (datasets of their own, see [Pack layers](#pack-layers)) | NGA's Vector Map Level 0 by theme: boundaries, hydrography, transportation, population, elevation, vegetation and other themes; hydrography, elevation and vegetation as four regional packs each; the ESRI-owned administrative areas apart. Details in [docs/data/vmap0.md](../data/vmap0.md) | 4–150 MB each |

A geoid pack replacing a base pack geoid makes every elevation source on
that datum use the finer grid (the registry keys providers by the grid in
use, so the terrain is reloaded when the pack is installed or removed).
One that brings a datum of its own adds it to the datums elevation files
and web terrain can be measured from (`ProviderRegistry::verticalDatums()`,
see [vertical-datums.md](vertical-datums.md)).

**Datum shift grids.** A pack may also carry horizontal datum shift
grids (PROJ GeoTIFF, such as OSTN15) for projections in another datum,
listed under the manifest's optional `"datumShifts"` (each with `id`,
`file`, `name`, `licence`, `attribution` and `source`, as a dataset
without `kind`), not among the datasets: versions 0.0.2 and earlier
reject a manifest with a dataset kind they don't know, but ignore an
unknown key, so the UK grids pack still installs there (OSGM15 only).
`setInstalledPacks()` passes them to `ts::setDatumShiftGrids()`; see
[projections.md](projections.md#the-uk-national-grid-and-the-osgb36-datum).

Their packager configurations are `tools/packager/packs/*.json`; `make packs`
builds them with their release files and index entries
([docs/tools/packager.md](../tools/packager.md#extra-packs)).

## Where the source data is kept

**Decided 2026-11-22.** Two repositories hold the packs' data, and every
pack, the base pack and any new one, uses them:

- **`marutan/terrasoar-packs-sources`** keeps the **raw source data**:
  every file a pack configuration's `"sources"` names, unchanged, with
  where it was downloaded from, when, its SHA-256 and the licence or terms
  it was distributed under. The files are release assets (several are
  larger than the 100 MB a git file may be), one release per dataset
  (`vmap0-5.0`, `gshhg-2.3.7`, …); the repository's `sources.json`
  catalogues them and `sources/<release>/` holds each one's `SOURCE.md`
  and licence texts, which its release also carries.
- **`marutan/terrasoar-packs`** keeps the **built packs** and their
  `index.json`, as before ([The first packs](#the-first-packs)).

The packager reads only from the first. Each source in a configuration has
its `"mirror"`, the release asset in `terrasoar-packs-sources`, which
`fetch_sources.py` downloads and checks against the pinned `"sha256"`,
and its `"url"`, where the file was first published, which the pack's
manifest records as the source. A source without a mirror in that
repository is an error, even when the file is already on disk.

**Why**: upstream sites move and disappear (NGA's WVS download went, and
VMAP0 is now only on a university's server), change files under the same
name, and rate-limit or block automated downloads. With every source kept
alongside its terms, any pack can be rebuilt exactly, years on, from one
place we control, and what each file may be used for is recorded next to
it.

**Adding a source** (for a new pack, or a new version of a source):

1. Pin it in the configuration with `"url"` (upstream) and `"sha256"`,
   and download it with `fetch_sources.py --upstream`.
2. Check its licence or terms allow it to be redistributed. Then create
   the release `<dataset>-<version>` in `marutan/terrasoar-packs-sources`
   with the file and its licence texts attached. Add `sources/<release>/`
   (`SOURCE.md` and the licence texts) and its entry in `sources.json`,
   and commit them.
3. Add `"mirror"` to the configuration, and build without `--upstream`.

A source whose terms don't allow redistribution can't be kept there, and
so can't be a pack's source.

## Variants

A dataset may instead be one of several pictures of a base pack dataset
chosen by a time-like value: `"variantOf": "bluemarble", "variant":
"2004-01"` (both optional, so the manifest format stays 1). The
`bluemarble-monthly` pack has twelve, one per month of 2004, from NASA's
5400 × 2700 monthly images tiled to level 3 (about 9.8 km per pixel,
24.9 MB in all). The registry keeps them by dataset and value
(`setInstalledPacks()`); a base pack source whose `dimensions` give
`["month", "<variant>"]` reads that variant's file and shows its
attribution, and any other value reads the dataset as usual
(`basePackFile()`). Base pack layers without time settings get them from
`basePackTime()` while variants are installed, following the clock's
month ([wms.md](wms.md#time)). The packager builds variants from
`"imageryVariants"`, a list of imagery entries each with `variantOf` and
`variant` ([docs/tools/packager.md](../tools/packager.md)).

## Pack layers

A pack may also bring data of its own rather than better data for the
base pack's layers: the VMAP0 packs (Phase 21, see
[docs/data/vmap0.md](../data/vmap0.md)). Such a dataset has no
`replaces`; layer sources name it by its own id, as they name a base pack
dataset (`{"kind": "basepack", "dataset": "vmap0-transportation"}`), and
the registry finds its file among the installed packs
(`setInstalledPacks()`). Two optional manifest keys, which older versions
ignore, offer its layers:

- `"name"`: the pack's name for the user, the title of its submenu;
- `"layers"`: layer definitions (`ts::LayerDefinition`'s JSON) whose
  sources name the pack's datasets, bottom first.

**Add Layer → Data Packs** lists every installed pack that has layers, a
submenu each: its layers one by one, and **All Layers**, which adds them
together as a collection named after the pack (the topmost first in the
tree, as drawn). The ids of the layers are `pack:<dataset>:<layer>`.
Projects keep the layers as they keep any other; with the pack removed
they show the error "… is in no installed data pack".

A pack layer's source may also give:

- `"tables"`: the dataset's tables to read, by name without the storage
  level (`"roads"` reads `roads_4` and `roads_6`), so one GeoPackage
  serves several layers;
- `"styleRules"`: the per-feature style rules ([wfs.md](wfs.md#per-feature-styles)),
  applied as the tiles load.

and its style `"labels"`: an attribute whose value labels the layer's
points on the view (VMAP0's place and airport names, spot heights). The
window asks the renderer which stored tiles it drew
(`VectorRenderer::drawnTiles()`) and the provider for their named points
(`FeatureProvider::labelPoints()`) a few times a second while such a layer
is shown, and hands them to the view with KML's labels, which thins them
where they crowd.

**Picking.** A GeoPackage with attributes keeps the 48 tiles it loaded
last; What Is Here lists the features under the click from them, finest
tiles first, each feature (by its `feature_id`) once, without the table
name.

### Regional packs

A theme too big for one download is split along VMAP0's libraries into
packs with the same dataset id and layers, and the same `"name"`
(`layerGroup` in the packager configuration): `vmap0-hydrography-noamer`,
`-eurnasia`, `-soamafr` and `-sasaus` all hold `vmap0-hydrography`. The
registry reads the files of every installed pack of a dataset together
(`ts::CompositeFeatureProvider`: a tile's features are those of each), so
one layer shows whichever regions are installed, and Add Layer lists the
theme once. The regions' tables are alike (the same levels), which the
composite needs; where two regions meet, each has its own features cut at
the library edge.

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
