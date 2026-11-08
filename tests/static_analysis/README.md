# Static-analysis self-test

`known_bad.cpp` contains deliberate mistakes: an uninitialised member, a
shadowed name, an unused variable and a temporary Qt container that is
detached.

```sh
tools/checks/run_static_analysis.py --self-test
```

runs clang-tidy, clazy and cppcheck on it and fails if any of them stops
reporting what `SELF_TEST_EXPECTED` in that script lists. That way a tool
that is missing, crashes, or has a broken configuration fails CI instead of
quietly reporting zero findings.

The file is never built. Don't fix it, and don't add it to a `.pro` file.
When you change the analysis configuration, keep the self-test passing; if a
tool renames a check, update `SELF_TEST_EXPECTED`.
