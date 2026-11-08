# Coding standards

These rules apply to all C++ in `core/`, `app/`, `tools/` and `tests/`.
Most are enforced by `tools/checks/run_all.py`, which CI runs on every push.
Run it before you push:

```sh
python3 tools/checks/run_all.py            # everything
python3 tools/checks/check_format.py --fix # reformat in place
```

| Check | Script | What fails |
| --- | --- | --- |
| Formatting | `check_format.py` | Any difference from `clang-format` 18 output |
| Include order | `check_include_order.py` | Includes out of group order, or groups not separated |
| No `auto` | `check_no_auto.py` | The `auto` keyword anywhere |
| Translatable strings | `check_translatable.py` | A literal passed to a user-visible API without `tr()` |
| Translations up to date | `tools/i18n/update_translations.py --check` | `terrasoar_en.ts` out of step with the code |
| API docs | `check_doxygen.py` | Any Doxygen warning, including undocumented public items |

Static analysis (clang-tidy, clazy, cppcheck) runs separately with
`make analyze`; see [Warnings and static analysis](#warnings-and-static-analysis).

## Language

- C++17. Qt 5.15 and Qt 6 both supported from the same code.
- **No `auto`**: write the type. This includes range-for loops, lambdas
  stored in variables (use `std::function` or a named type) and iterators.
- Deprecated Qt APIs are compile errors (`QT_DISABLE_DEPRECATED_BEFORE`).
  Use the API that exists in both Qt 5.15 and Qt 6. When the two differ,
  branch on `QT_VERSION` in one place and add a comment.
- No implicit `const char*` ↔ `QString` conversion (`QT_NO_CAST_FROM_ASCII`):
  write `QStringLiteral("…")` or `QLatin1String("…")`.
- Prefer RAII owners (`std::unique_ptr`, the `ts::` handle classes) over
  manual cleanup. Third-party handles get an owning wrapper in core.
- Warnings are errors in CI, and the code must stay at zero static-analysis
  findings. See [Warnings and static analysis](#warnings-and-static-analysis).

## Warnings and static analysis

### Compiler warnings

`common.pri` sets the same warnings for every build. `CONFIG+=werror` (used
by CI) makes them errors; local builds show them without failing. Fix the
warning; don't silence it locally.

| Compiler | Flags |
| --- | --- |
| GCC, Clang | `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wconversion -Wsign-conversion -Wold-style-cast -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough` |
| Clang only | `-Wnull-dereference` (GCC's version reports false positives in inlined standard-library code at `-O2`) |
| MSVC | `/W4` |

Qt's and the third-party libraries' headers are system or external headers
(`-isystem`; on MSVC `/external:anglebrackets /external:W0`), so warnings
cover only our own code. Include them with `<...>` and our own headers with
`"..."`, or MSVC treats them the wrong way round.

Don't use Qt Test's `QTRY_VERIFY`/`QTRY_COMPARE`: in Qt 6.8 they expand,
in our own code, to a `std::chrono` count narrowed to `int`, which GCC's
`-Wconversion` rejects. Write
`QVERIFY(QTest::qWaitFor([&]() { return condition; }, 5000))` instead.

With `-Wconversion` and `-Wsign-conversion`, write conversions out:
`static_cast<std::size_t>(i)`, `static_cast<double>(list.size())`,
`std::ptrdiff_t` offsets for iterators.

### Static analysis

Three tools run on every pull request (changed files) and nightly (the whole
tree). Any finding in our own code fails CI. Run them locally with
`make analyze`; see [building.md](building.md#static-analysis).

| Tool | Version | Configuration |
| --- | --- | --- |
| clang-tidy | 18 | `.clang-tidy` at the repo root; `tests/.clang-tidy` for test code |
| clazy | 1.11 | `CLAZY_CHECKS` in `tools/checks/run_static_analysis.py` |
| cppcheck | 2.13 | `CPPCHECK_ARGS` in `tools/checks/run_static_analysis.py` |
| MSVC `/analyze` | VS 2022 | `CONFIG+=msvc_analyze` (nightly) |

**clang-tidy** runs the `bugprone`, `cert`, `clang-analyzer`, `concurrency`,
`cppcoreguidelines`, `modernize`, `performance`, `portability` and
`readability` groups, plus the compiler warnings (`clang-diagnostic-*`).
Switched off, and why:

| Check | Why it is off |
| --- | --- |
| `modernize-use-auto` | Contradicts the no-`auto` rule |
| `modernize-use-trailing-return-type` | Style; the project writes the type first |
| `modernize-return-braced-init-list` | Style; `return QString();` names the type |
| `modernize-use-nodiscard` | Would mark nearly every getter; reconsider for error returns |
| `cppcoreguidelines-avoid-magic-numbers`, `readability-magic-numbers` | Geodesy and test values read better as literals; name constants in review |
| `readability-identifier-length` | `x`, `y`, `z`, `i`, `n` are normal in maths code |
| `cppcoreguidelines-owning-memory` | Qt's parent/child ownership (`new QLabel(this)`) has no `gsl::owner` |
| `cppcoreguidelines-pro-bounds-pointer-arithmetic`, `cppcoreguidelines-pro-type-reinterpret-cast` | Needed for the C APIs (GDAL, PROJ, GEOS, SQLite, OpenGL) and `argv`; easy to grep for and reviewed by hand |
| `performance-enum-size` | Micro-optimisation |
| `readability-convert-member-functions-to-static` (tests only) | Qt Test runs private slots, which can't be static |

Options: cognitive complexity ignores macros (Qt Test's `QVERIFY` expands to
nested `if`s), and `if (pointer)` is allowed.

**clazy** runs levels 0 to 2, without `qstring-ref` (it suggests `midRef()`,
which Qt 6 removed) and, in tests, without `ctor-missing-parent-argument`
(`QTEST_MAIN` creates the test object without a parent). clazy 1.11's
`range-loop-detach` doesn't recognise Qt 6's containers, so it can't catch a
range-for over a non-const `QList`; use `std::as_const()` anyway.

**cppcheck** runs with `--enable=warning,performance,portability
--inline-suppr --library=qt`. It reads only our own headers and knows Qt
through `--library=qt`.

Findings located inside Qt or other third-party headers (for example the
Clang static analyzer misreading `QWeakPointer`'s reference counting) are
listed as ignored, not counted. Errors that mean a tool could not analyse a
file always count.

### Suppressing a finding

Fix the code if you can. If a finding is wrong or can't be fixed, suppress
that one finding inline, with the reason on the line above:

```cpp
// glibc declares the rusage fields inside anonymous unions.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
const long maxRss = usage.ru_maxrss;

// m_buffer is filled by read() before any use.
// cppcheck-suppress uninitMemberVar
Reader::Reader() = default;
```

- Name the check: `NOLINT(check-name)`, never a bare `NOLINT`.
- `NOLINTNEXTLINE` covers only the next line. When clang-format splits the
  statement, wrap it in `NOLINTBEGIN(check-name)` / `NOLINTEND(check-name)`.
- clazy findings are suppressed with `// clazy:exclude=check-name`.
- Never switch a check off for the whole project without a reason in
  `.clang-tidy` (or the script) and in the tables above.

## Formatting

The repository's `.clang-format` defines the layout: 2-space indent,
80 columns, Linux brace style (function and class braces on their own
line), `T* p` and `T& r`. CI pins clang-format 18. Older versions format
some constructs differently, so set `CLANG_FORMAT=clang-format-18` if your
default differs.

## Naming

Qt style, with units and frames in names:

| Item | Style | Example |
| --- | --- | --- |
| Types, enums | `UpperCamelCase` | `FrameTimeStats`, `OpenMode` |
| Functions, variables | `lowerCamelCase` | `featureCount()`, `tileKey` |
| Members | `m_` prefix | `m_handle` |
| Constants | `lowerCamelCase` (`constexpr`) | `concurrentThreadCount` |
| Macros | `TS_UPPER_CASE` | `TS_VERSION_STRING` |
| Namespaces | `ts`, `ts::<sub>` | `ts::test` |
| Files | lower case, class name | `frametimestats.h` |

**Units and frames:** a value with a unit has it as a suffix: `heightM`,
`latDeg`, `lonRad`, `durationS`, `startupMs`, `sizeBytes`. Coordinates name
their frame: `positionEcef`, `centreGeodetic`. The Doxygen comment states
the unit and frame again.

Getters have no `get` prefix (`version()`, not `getVersion()`). The only
exception is `get()` returning a raw underlying handle, as on the
`std::unique_ptr`-like wrappers.

## Includes

Separate blocks, in this order, each separated by one blank line:

1. The file's own header (`.cpp` files only)
2. Standard library and system headers: `<vector>`, `<windows.h>`
3. Qt: `<QString>`, `<QtTest>`
4. Third-party: `<gdal.h>`, `<proj.h>`, `<geos_c.h>`, `<sqlite3.h>`
5. Project: `"ts/base/buildinfo.h"`, `"mainwindow.h"`

clang-format sorts the lines inside each block. Headers use include
guards named `TS_<PATH>_H` (e.g. `TS_DEPS_GEOSCONTEXT_H`). Forward-declare
in headers where that avoids an include.

Every file starts with `// SPDX-License-Identifier: GPL-2.0-or-later`.

## Documentation comments

Every public item in `core/include/` and `app/src/` has a `/** */`
Doxygen comment. Doxygen runs with warnings as errors.

- Every header has a `@file` comment saying what it contains.
- The first sentence is the brief. Write it as a statement:
  "Returns the …", "Owns one …".
- Document parameters with `@param` when the name and type don't say it
  all, and always give units and frames.
- Every public class or struct states its thread-safety level with one
  of the project aliases:
  - `@reentrant`: safe when each thread uses its own instances
  - `@threadsafe`: one instance may be shared between threads
  - `@guithread`: GUI thread only
  - `@renderthread`: render thread only, with its OpenGL context current
    (the renderer backend and GPU resources)
- Write hazards as `@warning` (see `ts::GeosContext`).

```cpp
/**
 * Rolling window of frame durations.
 *
 * @reentrant
 */
class FrameTimeStats
{
public:
  /** Adds one frame that took @p durationS seconds. */
  void addFrame(double durationS);
  …
};
```

## Thread safety

See [architecture.md](architecture.md#threading-model) for the model. In
code:

- No mutable globals or non-const statics. Use `std::call_once` or a
  function-local static for one-time initialisation.
- No locale- or global-state-dependent C functions: `strtok`, `setlocale`,
  `atof`, `strtod`, `localtime`. Parse numbers with `QLocale::c()` or
  `std::from_chars`. Watch for third-party functions that do this
  internally (GEOS WKT I/O does).
- Every new reentrant class gets a concurrent test with its own instances
  per thread. Every thread-safe service gets a shared-instance stress test.
  Both run under ThreadSanitizer in CI.
- Never add a TSan suppression for Terra Soar's own code; fix the race.
  Suppressions in `tests/tsan.supp` are only for uninstrumented third-party
  code, each with a comment explaining why.

## Translatable text

- Every user-visible string is translatable: `tr()` in QObject classes,
  `QCoreApplication::translate("context", …)` elsewhere, and
  `QT_TR_NOOP` / `QT_TRANSLATE_NOOP` for static tables.
- Use placeholders, never concatenation:
  `tr("%1 of %2 tiles").arg(done).arg(total)`. Use `%n` for plurals:
  `tr("%n layer(s)", nullptr, count)`.
- Format numbers, dates and units with `QLocale`, not `QString::number`,
  in anything the user sees.
- Set all widget text in a `retranslateUi()` that runs again on
  `QEvent::LanguageChange`, so the language can change at run time.
- Text that is deliberately not translated (an identifier, a file name)
  gets a `// ts-notr: <reason>` comment on the same line.
- After adding or changing strings, run `make lupdate` and commit the
  updated `translations/terrasoar_en.ts`. Plural entries need their English
  singular and plural forms filled in by hand (Qt Linguist or a text
  editor). The check fails until they are.

Tests and tools may print plain, untranslated text.
