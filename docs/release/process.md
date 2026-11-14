# Release process

How Terra Soar versions are numbered, what a release contains, and how one
is made. The packaging itself is in [packaging.md](packaging.md).

## Versions

- Versions follow [Semantic Versioning](https://semver.org/): MAJOR.MINOR.PATCH.
  Before 1.0.0 the minor version may change file formats and behaviour; a
  patch version only fixes things.
- **The first release is 0.0.1** (decided 2026-11-13; the roadmap's "1.0"
  is left for later).
- The version is `TS_VERSION` in `common.pri`, the only place it is set:
  the program (`buildInfo().version`), the About window, the installers'
  names and metadata and the source tarball's name come from it, and a
  release tag must match it (`v0.0.1` for 0.0.1; the release workflow
  fails otherwise).
- Pack versions are separate (`<source version>.<build>`, see
  [packs.md](../design/packs.md)), as is the base pack's (its manifest's
  `version`).

## What a release publishes

On the GitHub release `v<version>` of `marutan/terrasoar`:

| File | What |
| --- | --- |
| `TerraSoar-<version>-setup.exe` | Windows 10+ installer (NSIS), x64, with the base pack |
| `TerraSoar-<version>-x86_64.AppImage` | Linux AppImage, with the base pack |
| `TerraSoar-<version>.flatpak` | Linux Flatpak bundle (`io.github.marutan.TerraSoar`), with the base pack |
| `terrasoar-<version>.tar.gz` | Source tarball (`git archive` of the tag) |
| `*.sha256` | The SHA-256 of each of these |

The release notes are the version's section of `CHANGELOG.md`.

## Making a release

1. **Changelog.** Move the `[Unreleased]` entries under a new
   `## [<version>] – <YYYY-MM-DD>` heading in `CHANGELOG.md`, and update
   the links at the bottom.
2. **Version.** Set `TS_VERSION` in `common.pri`; update the download
   names in `docs/user/installation.md` and the `<release>` (version and
   date) in `packaging/linux/io.github.marutan.TerraSoar.metainfo.xml`.
   `python3 tools/release/check_release.py check v<version>` says what
   still disagrees.
3. **Pull request.** Open one with these changes; CI must be green. To
   rehearse the release, run the release workflow by hand on the branch
   (`workflow_dispatch`, a dry run: everything below but publishing; the
   files it would publish are its artefact `release-dry-run`).
4. **Tag.** After merging: `git tag -a v<version> -m "Terra Soar
   <version>"` on the merge commit, and push the tag. **Tagging publishes:**
   the maintainer decides when.
5. **The release workflow** (`.github/workflows/release.yml`, run by the
   tag) checks the version (`check_release.py check`: the tag is
   `v<TS_VERSION>`, the changelog section is there and dated, the metadata
   and guide name it), then runs, all on the tagged commit, the whole CI
   suite (`ci.yml`), the full suite (`nightly.yml`: golden images,
   performance, full static analysis, MSVC `/analyze`) and the packages
   workflow (`packages.yml`: AppImage, Flatpak and Windows installer, each
   built and install-tested). Only if every one passes does it create the
   GitHub release: the four packages and the source tarball, each with its
   `.sha256` (checked again before publishing), the notes the version's
   changelog section. If anything fails nothing is published; fix it,
   delete the tag (`git push --delete origin v<version>`), and tag again.
6. **Check** the published release: download each file, verify its
   checksum, install it and start it (the install tests did this in CI;
   this is the human look).

### Checklist

- [ ] `CHANGELOG.md` has the version, date and links
- [ ] `TS_VERSION`, installation guide and packaging metadata agree
- [ ] CI green on the release commit; a release dry run green
      (`check_release.py check` passing)
- [ ] Licence audit green (`tools/checks/check_notices.py`, part of
      `run_all.py`), and the packaging jobs' audit of bundled libraries
- [ ] Tag pushed; release workflow green; files and checksums on the
      release
- [ ] Each package downloaded, installed and started once by hand

## Signing

Release 0.0.1 is **not code-signed** (decided 2026-11-13): Windows
SmartScreen warns on the first runs of the installer (the
[installation guide](../user/installation.md#windows) says what to do).
The release workflow has a signing step for the Windows installer and the
program that runs only when the repository has the secrets it needs
(`WINDOWS_SIGNING_CERTIFICATE`, a base64 PFX, and
`WINDOWS_SIGNING_PASSWORD`; see [packaging.md](packaging.md#signing));
adding a
certificate later (SignPath Foundation, a commercial certificate or Azure
Trusted Signing) needs no other change. The Linux packages aren't signed;
their checksums are published with them.

## Licences

Every library and dataset in a package must be in `THIRD_PARTY_NOTICES`:
`tools/checks/check_notices.py` checks the libraries the project links and
the data sources the packager uses, and the packaging jobs check the
libraries they actually bundle. The About window shows the notices as
built into the program.
