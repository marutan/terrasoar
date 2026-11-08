// SPDX-License-Identifier: GPL-2.0-or-later
//
// Known-bad code for the static-analysis self-test:
//
//   tools/checks/run_static_analysis.py --self-test
//
// Every problem below is deliberate. Each tool must keep reporting the ones
// listed in SELF_TEST_EXPECTED in that script, so a broken tool or config
// fails CI instead of passing silently. This file is never built or linked;
// don't fix it, and don't add it to a .pro file.
#include <QList>

namespace ts::analysis_self_test
{

// Uninitialised member (clang-tidy cppcoreguidelines-pro-type-member-init,
// cppcheck uninitMemberVar).
class UninitialisedMember
{
public:
  UninitialisedMember() {}

  int value() const { return m_value; }

private:
  int m_value;
};

// Shadowed name (-Wshadow) and unused variable (-Wunused-variable), both
// reported by clang-tidy as clang-diagnostic-*.
int shadowed(int count)
{
  int total = 0;
  {
    int count = 2;
    total += count;
  }
  int unused = 0;
  return total + count;
}

QList<int> makeList();

// Non-const call on a temporary Qt container, which detaches it (clazy
// detaching-temporary).
int detaches()
{
  return makeList().first();
}

} // namespace ts::analysis_self_test
