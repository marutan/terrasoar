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
- Warnings are errors in CI: GCC/Clang with `-Wall -Wextra -Wpedantic
  -Wshadow -Wnon-virtual-dtor`, MSVC with `/W4`. Fix the warning. Don't
  silence it locally.

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
