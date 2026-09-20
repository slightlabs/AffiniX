#!/usr/bin/env python3
"""check_header_hygiene.py — DESIGN.md's "no detail:: in a public signature"
rule (IMPLEMENTATION_PLAN.md M0-07 / M1-03's sibling script).

`namespace afx::detail` holds implementation types that are not part of the
public API and carry no stability guarantee. A `detail::` type leaking into a
public return type, parameter, or data member forces callers to name (and
therefore depend on) an unstable type. This script greps public headers for
that leak.

What counts as "public": anything outside a `private:`/`protected:` section
(structs default to public, classes default to private) and outside a
`namespace detail { ... }` block — the latter is exempt because it *is* the
detail namespace, not a leak from it.

Known limitation: this is a regex heuristic, not a parser. It reliably
catches `detail::Type name` / `detail::Type& name(` shapes, but a detail::
type hidden behind another template one level down (e.g.
`std::shared_ptr<detail::ChannelState<T>>`) is not detected — flagging that
needs real template-aware parsing. Treat a clean run as "no obvious leak",
not a proof of API cleanliness.

Usage:
    check_header_hygiene.py [--root DIR] [FILE ...]

Exit status: 0 if clean, 1 if any violation was found (printed to stderr).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# A qualified detail:: token, matched on its own so its trailing `\w+` is
# never pressured to backtrack into fewer characters by whatever follows (a
# single combined regex for "type then declarator name" both bugs out on
# `detail::tls_ambient = x` by splitting "tls_ambient" into a fake type +
# declarator pair). What immediately follows a token is inspected separately
# in _find_declarator.
_DETAIL_TOKEN_RE = re.compile(r"\bdetail::\w+(?:<[^<>;{}]*>)?")
# `<ws> [&*]{0,2} <ws> identifier`, i.e. the declarator name of a
# `detail::Type [&|*] name` declaration. A call (`detail::f(...)`) or a plain
# reference to a detail:: symbol (`detail::tls_ambient = ...`) never has this
# shape immediately after the qualified name.
_DECLARATOR_RE = re.compile(r"\s*[&*]{0,2}\s*([A-Za-z_]\w*)")
_ACCESS_RE = re.compile(r"^\s*(public|protected|private)\s*:")
_CLASS_RE = re.compile(r"\b(class|struct)\b[^;{]*\{")
_NAMESPACE_RE = re.compile(r"\bnamespace\s+([A-Za-z_:]\w*)?\s*\{")
_LINE_COMMENT_RE = re.compile(r"//.*$")


class _Scope:
    __slots__ = ("kind", "access", "is_detail_ns")

    def __init__(self, kind: str, access: str, is_detail_ns: bool = False):
        self.kind = kind              # "class", "struct", "namespace", "other"
        self.access = access          # "public" or "private"
        self.is_detail_ns = is_detail_ns


def _strip_comment(line: str) -> str:
    return _LINE_COMMENT_RE.sub("", line)


def find_violations(text: str, path: str) -> list[str]:
    violations: list[str] = []
    # Namespace-scope (and struct) default is public; class default is
    # private. The stack tracks the innermost scope's current access and
    # whether we are inside `namespace detail`.
    stack = [_Scope("namespace", "public")]

    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = _strip_comment(raw_line)
        stripped = line.strip()
        if not stripped:
            continue

        # Access-specifier labels flip the current (innermost) scope.
        m = _ACCESS_RE.match(stripped)
        if m:
            stack[-1].access = m.group(1)
            continue

        # A class/struct/namespace opening on this line pushes a new scope.
        # This is intentionally line-local (matches this codebase's style of
        # opening braces on the same line as the keyword); multi-line class
        # headers are rare enough not to special-case here.
        ns_m = _NAMESPACE_RE.search(stripped)
        cls_m = _CLASS_RE.search(stripped)
        pushed = False
        if ns_m:
            name = ns_m.group(1) or ""
            in_detail = stack[-1].is_detail_ns or name == "detail"
            stack.append(_Scope("namespace", "public", in_detail))
            pushed = True
        elif cls_m:
            kind = cls_m.group(1)
            default_access = "public" if kind == "struct" else "private"
            stack.append(_Scope(kind, default_access, stack[-1].is_detail_ns))
            pushed = True

        current = stack[-1] if not pushed else stack[-2]
        # Only inspect lines in a currently-public scope, outside detail::.
        if current.access == "public" and not current.is_detail_ns:
            for tok in _DETAIL_TOKEN_RE.finditer(stripped):
                rest = stripped[tok.end():]
                if rest.startswith("::"):
                    continue  # further-qualified name, not a declarator
                decl = _DECLARATOR_RE.match(rest)
                if not decl:
                    continue  # a call or a bare reference, not a declaration
                violations.append(
                    f"{path}:{lineno}: public signature references "
                    f"{tok.group(0)!r} {decl.group(1)!r} — detail:: types "
                    f"must not appear in public signatures"
                )

        # Naive brace bookkeeping: pop one scope per unmatched closing brace.
        # We don't track full nesting depth (would need a real tokenizer);
        # instead, every `}` at the start of a dedented line closes the
        # innermost pushed scope. This is a heuristic, not a parser — see the
        # module docstring.
        opens = raw_line.count("{") - (1 if pushed else 0)
        closes = raw_line.count("}")
        for _ in range(opens):
            stack.append(_Scope("other", stack[-1].access, stack[-1].is_detail_ns))
        for _ in range(closes):
            if len(stack) > 1:
                stack.pop()

    return violations


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default="include/afx",
                     help="directory to scan when no FILEs are given")
    ap.add_argument("files", nargs="*", metavar="FILE")
    args = ap.parse_args(argv)

    if args.files:
        paths = [Path(f) for f in args.files]
    else:
        root = Path(args.root)
        paths = sorted(root.rglob("*.hpp")) if root.is_dir() else []

    all_violations: list[str] = []
    for path in paths:
        try:
            text = path.read_text()
        except OSError as e:
            print(f"error: cannot read {path}: {e}", file=sys.stderr)
            return 2
        all_violations.extend(find_violations(text, str(path)))

    if all_violations:
        for v in all_violations:
            print(v, file=sys.stderr)
        print(f"\n{len(all_violations)} header hygiene violation(s)",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
