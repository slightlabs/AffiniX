# Contributing

AffiniX is built against a written plan
([docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md)) and a written
design ([docs/DESIGN.md](docs/DESIGN.md), plus the [ADRs](docs/adr/)). Read
those first; this file is the condensed, day-to-day version of
IMPLEMENTATION_PLAN.md §2 ("working agreements").

## Ground rules

1. **One branch per milestone**, PRs per task or small task group. `main`
   stays green: every merge builds on GCC and Clang, and passes every test
   that exists at that point.
2. **No public API lands without a test and a use in an example.** An
   interface with no caller is an interface shaped by guessing.
3. **Deviating from DESIGN.md requires a doc change in the same PR.** If the
   deviation touches a recorded decision, it needs an ADR amendment or a new
   ADR that supersedes it.
4. **Annotations are not optional.** A new public, EM-affine member needs
   `AFX_REQUIRES` (or an explicit `AFX_NO_TSA` with a reason) — see
   [ADR-0009](docs/adr/0009-compile-time-affinity-annotations.md). CI enforces
   this with `tools/check_annotations.py`; see that script's docstring for
   how the pre-existing debt in `tools/annotations_allowlist.txt` is tracked
   without silently growing.
5. **Benchmarks accompany performance-relevant work**, with a recorded
   baseline, in the same PR.
6. **Every bug fixed gets a test that fails without the fix.**
7. **Commit messages explain why.** Put the task ID (e.g. `M4-03`) in the
   body, not the subject.

## Before you open a PR

```sh
cmake --preset gcc-debug   # or clang-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug --output-on-failure

python3 tools/check_header_hygiene.py
python3 tools/check_annotations.py
python3 tools/test_hygiene_scripts.py   # proves the two scripts above still work
```

If you touched anything under `include/afx/net` or `include/afx/core`, also
run the ASan/UBSan preset:

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

## Code style

- Formatting is `.clang-format` (run `clang-format -i` on changed files);
  linting is `.clang-tidy`. Neither has been run over the pre-existing tree
  yet (no clang toolchain was available to validate it in the environment
  this was authored in) — don't be surprised if CI's format-check job needs
  a one-time repo-wide reformat commit before it can be made blocking.
- Follow the surrounding file's conventions before reaching for a personal
  preference: 4-space indents, attached braces, `snake_case` for functions
  and members, `PascalCase` for types.
- Don't add `detail::` types to a public signature (`tools/check_header_hygiene.py`
  checks this, with the limitations documented in its docstring).

## Definition of done

See IMPLEMENTATION_PLAN.md §3. In short: builds warning-free on GCC and
Clang at `-Wall -Wextra -Wpedantic`; unit tests cover the stated behaviour
and at least one failure path; public headers carry annotations; debug
builds assert `is_current()` on every affine entry point; DESIGN.md is
updated if the implementation differs from it.
