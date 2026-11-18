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
/**
 * @file
 * Where the program keeps the user's settings (window sizes, the user
 * guide's last page and similar).
 */
#ifndef TS_APP_APPSETTINGS_H
#define TS_APP_APPSETTINGS_H

#include <memory>

#include <QString>

class QSettings;

namespace ts
{

/**
 * Returns the settings file: `terrasoar.ini` in the per-user
 * configuration directory (QStandardPaths::AppConfigLocation), so tests
 * running with QStandardPaths' test mode use a file of their own.
 */
QString settingsFilePath();

/** Opens the settings file (INI format) for reading and writing. */
std::unique_ptr<QSettings> openSettings();

} // namespace ts

#endif // TS_APP_APPSETTINGS_H
