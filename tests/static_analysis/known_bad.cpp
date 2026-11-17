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

//
// Known-bad code for the static-analysis self-test:
//
//   tools/checks/run_static_analysis.py --self-test
//
// Every problem below is deliberate. Each tool must keep reporting the ones
// listed in SELF_TEST_EXPECTED in that script, so a broken tool or config
// fails CI instead of passing silently. This file is never built or linked;
// don't fix it, and don't add it to a .pro file.
#include <cstdint>

#include <QList>

namespace ts::analysis_self_test
{

// Uninitialised member (clang-tidy cppcoreguidelines-pro-type-member-init,
// cppcheck uninitMemberVar).
class UninitialisedMember
{
public:
  UninitialisedMember() {}

  int32_t value() const { return m_value; }

private:
  int32_t m_value;
};

// Shadowed name (-Wshadow) and unused variable (-Wunused-variable), both
// reported by clang-tidy as clang-diagnostic-*.
int32_t shadowed(int32_t count)
{
  int32_t total = 0;
  {
    int32_t count = 2;
    total += count;
  }
  int32_t unused = 0;
  return total + count;
}

QList<int32_t> makeList();

// Non-const call on a temporary Qt container, which detaches it (clazy
// detaching-temporary).
int32_t detaches()
{
  return makeList().first();
}

} // namespace ts::analysis_self_test
