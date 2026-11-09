#!/usr/bin/env python3
"""Catch, in the lines a branch adds, two Windows build breaks that GCC and
Clang accept, before regress prepush spends ten minutes on an MSVC build:

  - an identifier <windows.h> defines away as a macro: near, far (minwindef.h),
    small (rpcndr.h) or interface (objbase.h). Wherever windows.h is reachable,
    `const auto near = ...` becomes `const auto = ...`.
  - a constexpr variable declared inside a function or a class without
    static. MSVC rejects a lambda without a capture default that reads one
    (C3493); static constexpr needs no capture, and inline constexpr is the
    namespace-scope spelling.

Only added lines are read, so code already on main is never flagged.

Usage: msvc-hazards.py [--base <rev>]   lines added since the merge base of
                                        HEAD and <rev> (default origin/main)
       msvc-hazards.py --self-test
Exit status: 0 clean, 1 a hazard was found, 2 the diff could not be read.
"""

import re
import subprocess
import sys

SUFFIXES = ("*.cpp", "*.cc", "*.cxx", "*.h", "*.hpp", "*.hh", "*.ipp", "*.mm")

MACRO_NAMES = re.compile(r"\b(near|far|small|interface)\b")
# A constexpr variable starting an indented line, or a statement after { or ;
# on one: static or inline before it is the fix, and if constexpr is no variable.
LOCAL_CONSTEXPR = re.compile(r"(?:^\s+|[;{]\s*)((?:static|inline)\s+)*(if\s+)?constexpr\s+"
                             r"[^;(=]*?\b[A-Za-z_]\w*\s*(?:=|\{)")


def code_of(line, in_block):
    """The line with comments and string or character literals blanked, and
    whether a block comment is still open at its end."""
    out, i, n = [], 0, len(line)
    while i < n:
        if in_block:
            end = line.find("*/", i)
            if end < 0:
                return "".join(out), True
            i, in_block = end + 2, False
            continue
        c = line[i]
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            in_block, i = True, i + 2
            continue
        if c in "\"'":
            j = i + 1
            while j < n and line[j] != c:
                j += 2 if line[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out), in_block


def hazards_in(line, in_block):
    """(messages, in_block) for one added line."""
    code, in_block = code_of(line, in_block)
    found = []
    for name in sorted({match.group(1) for match in MACRO_NAMES.finditer(code)}):
        found.append(f"'{name}' is a macro under <windows.h>; rename it")
    if any(not match.group(2) and not re.search(r"\b(?:static|inline)\b", match.group(0))
           for match in LOCAL_CONSTEXPR.finditer(code)):
        found.append("a constexpr local without static: MSVC will not let a lambda without a "
                     "capture default read it (C3493); make it static constexpr "
                     "(inline constexpr at namespace scope)")
    return found, in_block


def added_lines(base):
    """(path, line number, text) for each line HEAD adds since the merge base."""
    merge_base = subprocess.run(["git", "merge-base", "HEAD", base],
                                capture_output=True, text=True)
    if merge_base.returncode != 0:
        raise RuntimeError(f"no merge base between HEAD and {base}")
    diff = subprocess.run(["git", "diff", "--no-color", "-U0", merge_base.stdout.strip(), "HEAD",
                           "--", *SUFFIXES], capture_output=True, text=True)
    if diff.returncode != 0:
        raise RuntimeError(diff.stderr.strip() or "git diff failed")
    path, number = None, 0
    for text in diff.stdout.splitlines():
        if text.startswith("+++ "):
            path = text[6:] if text.startswith("+++ b/") else None
        elif text.startswith("@@"):
            number = int(re.search(r"\+(\d+)", text).group(1))
        elif text.startswith("+") and path:
            yield path, number, text[1:]
            number += 1


def check(base):
    found, in_block, last_path = 0, False, None
    for path, number, text in added_lines(base):
        if path != last_path:
            in_block, last_path = False, path
        messages, in_block = hazards_in(text, in_block)
        for message in messages:
            print(f"{path}:{number}: {message}\n    {text.strip()}")
            found += 1
    if found:
        print(f"msvc-hazards: {found} hazard(s) in lines added since {base}")
        return 1
    print(f"msvc-hazards: OK (lines added since {base})")
    return 0


def self_test():
    cases = [
        ("    const auto near = [] (double a, double b) { return a < b; };", 1),
        ("    constexpr int kRecordedKey = 50, kAuditionedKey = 70;", 1),
        ("    constexpr std::int64_t loopStart = 500;", 1),
        ("    constexpr std::array<int, 2> pair { 1, 2 };", 1),
        ("    static constexpr int kRecordedKey = 50;", 0),
        ("    constexpr static int kA = 1;", 0),
        ("    constexpr int inlineCount = 1;", 1),
        ("    inline constexpr int kLimit = 4;", 0),
        ("    if constexpr (std::is_same_v<T, int>) {", 0),
        ("    constexpr bool releaseStructural (int bytes) noexcept", 0),
        ("constexpr int kTopLevel = 1;", 0),
        ("    // keep the near and far clip planes apart", 0),
        ('    const char* label = "near far small interface";', 0),
        ("    int x = 1; /* far */ int y = 2;", 0),
        ("    auto interfaceCount = 3;", 0),
        ("    struct interface;", 1),
        ("    int probe() { const auto near = 1; constexpr int kLocal = 2; return near + kLocal; }", 2),
        ("    for (;;) { static constexpr int kOk = 1; break; }", 0),
        ("    template <typename T> void f() { if constexpr (sizeof (T) > 1) {} }", 0),
    ]
    failures = 0
    for line, expected in cases:
        got = len(hazards_in(line, False)[0])
        if got != expected:
            failures += 1
            print(f"self-test: expected {expected} hazard(s), got {got}: {line}")
    block, open_after = hazards_in("    /* near", False)
    closed, _ = hazards_in("    far */ int y = 2;", open_after)
    if block or closed or not open_after:
        failures += 1
        print("self-test: a block comment across lines was not skipped")
    print("msvc-hazards self-test: " + ("OK" if failures == 0 else f"{failures} failure(s)"))
    return 0 if failures == 0 else 1


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    base = "origin/main"
    if len(argv) == 3 and argv[1] == "--base":
        base = argv[2]
    elif len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        return check(base)
    except RuntimeError as error:
        print(f"msvc-hazards: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
