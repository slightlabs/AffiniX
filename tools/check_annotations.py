#!/usr/bin/env python3
"""check_annotations.py — ADR-0009 / working-agreement 4: "a new public
non-thread-safe member without AFX_REQUIRES fails CI".

A method is treated as EM-affine (non-thread-safe) if its body calls
AFX_ASSERT_CURRENT(...) — that macro *is* the runtime form of the same
invariant AFX_REQUIRES expresses at compile time (sys/annotations.hpp), so a
method using one without the other is only half-checked. This script flags
any public method whose declaration line lacks AFX_REQUIRES(...) /
AFX_NO_TSA but whose body calls AFX_ASSERT_CURRENT.

Pre-existing violations are tracked in an allow-list
(tools/annotations_allowlist.txt, `path:function` per line) rather than
silently ignored, so this check is "real" from day one (new violations
anywhere else fail CI) without blocking the build on debt that predates the
check. Shrinking the allow-list is a tracked M1 follow-up (ADR-0009 is a
one-way door: burn debt down, don't widen the list).

This is a bracket-counting heuristic, not a parser: it assumes (as this
codebase does) that a function's signature and opening `{` share a line, and
finds the matching `}` by counting braces in the raw text. Multi-line
signatures or braces inside string/char literals would confuse it; neither
occurs in this header-only style.

Usage:
    check_annotations.py [--root DIR] [--allowlist FILE] [FILE ...]

Exit status: 0 if no *new* (non-allow-listed) violations, 1 otherwise.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

_ACCESS_RE = re.compile(r"^\s*(public|protected|private)\s*:")
_CLASS_RE = re.compile(r"\b(class|struct)\b[^;{]*\{\s*$")
_ASSERT_RE = re.compile(r"\bAFX_ASSERT_CURRENT\s*\(")
_REQUIRES_OR_OPTOUT_RE = re.compile(r"\bAFX_(REQUIRES|NO_TSA)\b")
_CONTROL_KEYWORDS = {"if", "while", "for", "switch", "catch", "return"}
_DECL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# A definition line: has an opening brace at (or near) the end, is not a
# declaration-only statement, and isn't a control-flow block.
_DEF_END_RE = re.compile(r"\)[^(){};]*\{\s*$")


def _strip_comment(line: str) -> str:
    idx = line.find("//")
    return line if idx < 0 else line[:idx]


def _public_by_line(lines: list[str]) -> list[bool]:
    """Per-line "is this in a currently-public scope" flag. Classes default
    to private, structs and namespace scope default to public; an
    access-specifier label flips the *innermost* class/struct scope."""
    result = []
    # Stack of (is_class_like, access). Namespace/global scope is
    # represented as a non-class-like entry that is always "public" and
    # never flipped by an access label appearing inside it.
    stack = [("ns", "public")]
    for raw in lines:
        line = _strip_comment(raw)
        stripped = line.strip()

        m = _ACCESS_RE.match(stripped)
        if m and stack[-1][0] == "class":
            stack[-1] = ("class", m.group(1))

        result.append(stack[-1][1] == "public")

        cls_m = _CLASS_RE.search(stripped)
        opens = stripped.count("{")
        closes = stripped.count("}")
        if cls_m:
            kind = cls_m.group(1)
            stack.append(("class", "public" if kind == "struct" else "private"))
            opens -= 1
        for _ in range(opens):
            stack.append(("ns", stack[-1][1]))
        for _ in range(closes):
            if len(stack) > 1:
                stack.pop()
    return result


def _function_name(line: str) -> str | None:
    name = None
    for m in _DECL_RE.finditer(line):
        if m.group(1) in _CONTROL_KEYWORDS:
            continue
        name = m.group(1)
    return name


def find_violations(text: str) -> list[tuple[int, str]]:
    """Returns (1-based line number, function name) for each violation."""
    lines = text.splitlines()
    public = _public_by_line(lines)
    violations: list[tuple[int, str]] = []

    i = 0
    n = len(lines)
    while i < n:
        raw = lines[i]
        stripped = _strip_comment(raw).strip()
        is_def = (
            "(" in stripped and _DEF_END_RE.search(stripped)
            and _function_name(stripped) is not None
        )
        if not is_def:
            i += 1
            continue

        name = _function_name(stripped)
        has_requires = bool(_REQUIRES_OR_OPTOUT_RE.search(stripped))
        is_public = public[i]

        # Find the matching closing brace by counting braces from here.
        depth = 0
        has_assert = False
        j = i
        started = False
        while j < n:
            body_line = _strip_comment(lines[j])
            for ch in body_line:
                if ch == "{":
                    depth += 1
                    started = True
                elif ch == "}":
                    depth -= 1
            if _ASSERT_RE.search(body_line):
                has_assert = True
            if started and depth == 0:
                break
            j += 1

        if is_public and has_assert and not has_requires:
            violations.append((i + 1, name))

        i = j + 1

    return violations


def load_allowlist(path: Path) -> set[str]:
    if not path.exists():
        return set()
    out = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.add(line)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default="include/afx")
    ap.add_argument("--allowlist",
                     default=str(Path(__file__).parent / "annotations_allowlist.txt"))
    ap.add_argument("files", nargs="*", metavar="FILE")
    args = ap.parse_args(argv)

    if args.files:
        paths = [Path(f) for f in args.files]
    else:
        root = Path(args.root)
        paths = sorted(root.rglob("*.hpp")) if root.is_dir() else []

    allowed = load_allowlist(Path(args.allowlist))

    new_violations: list[str] = []
    for path in paths:
        text = path.read_text()
        for lineno, fn in find_violations(text):
            key = f"{path.as_posix()}:{fn}"
            if key in allowed:
                continue
            new_violations.append(f"{path}:{lineno}:{fn}")

    if new_violations:
        for v in new_violations:
            print(f"{v}: public, EM-affine (AFX_ASSERT_CURRENT) member "
                  f"has no AFX_REQUIRES/AFX_NO_TSA — see ADR-0009",
                  file=sys.stderr)
        print(f"\n{len(new_violations)} annotation violation(s) not in "
              f"{args.allowlist}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
