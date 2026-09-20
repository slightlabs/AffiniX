#!/usr/bin/env python3
"""Fixture tests for check_header_hygiene.py and check_annotations.py.

M0-07 requires these scripts to be "proven to fail when fed a deliberate
violation — a fixture test for the scripts themselves, because an unverified
lint script is a placebo." This is that fixture test.

Run directly, from the repository root (some cases resolve include/afx
relative to the current directory):

    python3 tools/test_hygiene_scripts.py

Exit status: 0 if every case behaved as expected, 1 otherwise.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import check_annotations       # noqa: E402
import check_header_hygiene    # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"

_FAILURES: list[str] = []


def _check(name: str, condition: bool, detail: str = "") -> None:
    status = "ok" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" — {detail}" if detail and not condition else ""))
    if not condition:
        _FAILURES.append(name)


def test_hygiene_catches_violation() -> None:
    text = (FIXTURES / "hygiene_violation.hpp").read_text()
    violations = check_header_hygiene.find_violations(
        text, "hygiene_violation.hpp")
    _check("hygiene: fixture violation is caught", len(violations) == 2,
           f"found {len(violations)}: {violations}")


def test_hygiene_accepts_clean() -> None:
    text = (FIXTURES / "hygiene_clean.hpp").read_text()
    violations = check_header_hygiene.find_violations(text, "hygiene_clean.hpp")
    _check("hygiene: clean fixture has no violations", violations == [],
           f"found: {violations}")


def test_hygiene_real_headers_clean() -> None:
    rc = check_header_hygiene.main(["--root", "include/afx"])
    _check("hygiene: real include/ tree is clean", rc == 0)


def test_annotations_catches_violation() -> None:
    text = (FIXTURES / "annotations_violation.hpp").read_text()
    violations = check_annotations.find_violations(text)
    _check("annotations: fixture violation is caught",
           violations == [(13, "spin")], f"found: {violations}")


def test_annotations_accepts_requires_and_no_tsa() -> None:
    text = (FIXTURES / "annotations_clean.hpp").read_text()
    violations = check_annotations.find_violations(text)
    _check("annotations: AFX_REQUIRES/AFX_NO_TSA both satisfy the check",
           violations == [], f"found: {violations}")


def test_annotations_real_headers_pass_allowlist() -> None:
    rc = check_annotations.main(["--root", "include/afx",
                                  "--allowlist", "tools/annotations_allowlist.txt"])
    _check("annotations: real include/ tree passes with today's allow-list",
           rc == 0)


def main() -> int:
    for fn in (
        test_hygiene_catches_violation,
        test_hygiene_accepts_clean,
        test_hygiene_real_headers_clean,
        test_annotations_catches_violation,
        test_annotations_accepts_requires_and_no_tsa,
        test_annotations_real_headers_pass_allowlist,
    ):
        fn()

    if _FAILURES:
        print(f"\n{len(_FAILURES)} fixture test(s) failed: {_FAILURES}",
              file=sys.stderr)
        return 1
    print(f"\nall fixture tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
