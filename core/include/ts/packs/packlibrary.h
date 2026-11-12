// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The directory holding installed extra packs and their downloads.
 */
#ifndef TS_PACKS_PACKLIBRARY_H
#define TS_PACKS_PACKLIBRARY_H

#include <optional>
#include <vector>

#include <QString>

#include "ts/packs/packindex.h"
#include "ts/packs/packmanifest.h"

namespace ts
{

/** An installed pack, see PackLibrary::installed(). */
struct InstalledPack {
  /** Its directory. */
  QString directory;
  /** Its manifest (only the id is certain if damaged and unreadable). */
  PackManifest manifest;
  /** True if a file is missing or of the wrong size: not to be used. */
  bool damaged = false;
  /** What is wrong (English, for logs), if damaged. */
  QString problem;
};

/** A download in the staging area, see PackLibrary::pending(). */
struct PendingPack {
  /** The index entry it was started from. */
  PackIndexEntry entry;
  /** Its staging directory. */
  QString directory;
  /** True if every file was checked and only the swap is left. */
  bool ready = false;
};

/**
 * The directory of extra packs (`<AppLocalDataLocation>/packs`):
 *
 * - `<id>/`: an installed pack (a manifest and its files);
 * - `.staging/<id>/`: a download, with `pending.json` (the index entry and
 *   whether it is ready), the manifest as `staged-manifest.json`, and the
 *   files, unfinished ones as `<path>.part`;
 * - `.trash/`: what is left to delete.
 *
 * Only renames and deletes; PackInstall fills the staging directories.
 * Not thread-safe: use it from one thread (the GUI's).
 *
 * See docs/design/packs.md.
 *
 * @reentrant
 */
class PackLibrary
{
public:
  /** Uses @p root (created when needed). */
  explicit PackLibrary(QString root);

  /** Returns the root directory. */
  const QString& root() const { return m_root; }

  /**
   * Start-up housekeeping, before any pack is opened: empties the trash,
   * deletes packs marked removed, and finishes ready swaps. Returns
   * problems (English, for logs).
   */
  std::vector<QString> tidy();

  /**
   * Returns the installed packs, sorted by id, after checking that each
   * file exists with its listed size (not its checksum).
   */
  std::vector<InstalledPack> installed() const;

  /** Returns the downloads in the staging area, sorted by id. */
  std::vector<PendingPack> pending() const;

  /** Returns the directory pack @p id is (or would be) installed in. */
  QString packDirectory(const QString& id) const;

  /** Returns the staging directory of pack @p id. */
  QString stagingDirectory(const QString& id) const;

  /**
   * Creates the staging directory for @p entry, or reuses the one there
   * if it is for the same version (a resumed download; otherwise it is
   * emptied first), and records the entry in its `pending.json`. Returns
   * the directory, or an empty string and @p errorString (English).
   */
  QString prepareStaging(const PackIndexEntry& entry, QString* errorString);

  /** Deletes the staging directory of pack @p id (a cancelled download). */
  void discardStaging(const QString& id);

  /**
   * Installs the finished download of pack @p id: marks it ready, puts its
   * manifest in place, moves an installed version to the trash and the
   * download in its place. Returns false with @p errorString (English) if
   * a rename failed (a file in use on Windows): the download is then kept
   * ready, and tidy() finishes it at the next start.
   */
  bool activate(const QString& id, QString* errorString);

  /**
   * Removes the installed pack @p id. Whatever can't be deleted now stays
   * marked, out of installed(), for tidy() to delete at the next start.
   * Returns true if it is gone from installed().
   */
  bool remove(const QString& id);

  /** Name of the staged manifest in a staging directory. */
  static QString stagedManifestName();

private:
  bool swapIn(const QString& id, QString* errorString);
  QString trashSlot(const QString& id) const;

  QString m_root;
};

} // namespace ts

#endif // TS_PACKS_PACKLIBRARY_H
