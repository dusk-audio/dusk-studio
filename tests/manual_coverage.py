#!/usr/bin/env python3
"""Hold MANUAL.md to its tests.

Every behaviour the manual documents has a row in tests/manual_coverage.tsv
naming what proves it, or saying why nothing automated can. This check fails
when:

  - a manual heading has no row, or a row names a heading the manual lacks;
  - a row cites a test, scenario, runner leg, ctest or script that does not
    exist;
  - a row's status and its evidence or note disagree;
  - the number of gap or partial rows differs from the ceiling recorded in
    the file. Covering a behaviour means lowering a ceiling in the same change;
    a new gap means raising one by hand, where review can see it.

Usage: manual_coverage.py <repo-root>
"""

import re
import sys
from pathlib import Path

STATUSES = {"auto", "partial", "manual", "gap", "n/a"}
EVIDENCE_KINDS = {"test", "scenario", "bb", "ctest", "script"}


def fail(errors):
    for e in errors:
        print(f"FAIL: {e}", file=sys.stderr)
    sys.exit(1)


def manual_headings(text):
    """Heading keys in document order: 'Chapter', 'Chapter > Section',
    'Chapter > Section > Subsection'. Front matter and fenced code are
    skipped, so a shell comment inside a code block is not a chapter."""
    lines = text.splitlines()
    i = 0
    if lines and lines[0].strip() == "---":
        i = 1
        while i < len(lines) and lines[i].strip() != "---":
            i += 1
        i += 1
    keys, fenced = [], False
    chapter = section = None
    children = {}
    for line in lines[i:]:
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        m = re.match(r"^(#{1,3}) (.+?)\s*$", line)
        if not m:
            continue
        level, title = len(m.group(1)), m.group(2)
        if level == 1:
            chapter, section = title, None
            children.setdefault(chapter, 0)
            keys.append((1, chapter))
        elif level == 2:
            section = title
            children[chapter] = children.get(chapter, 0) + 1
            keys.append((2, f"{chapter} > {section}"))
        else:
            keys.append((3, f"{chapter} > {section} > {title}"))
    # A chapter needs its own row only when it has no sections to carry one.
    required = [k for lvl, k in keys if lvl > 1 or children.get(k, 0) == 0]
    return required, {k for _, k in keys}


def test_case_names(root):
    names = set()
    for f in list((root / "tests").glob("*.cpp")) + list((root / "tests").glob("*.mm")):
        src = f.read_text(errors="replace")
        for m in re.finditer(r'TEST_CASE\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', src):
            names.add("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))))
    return names


def scenario_names(root):
    names = set()
    for f in (root / "src/engine/scenario/cases").glob("*.cpp"):
        names.update(re.findall(r'Scenario\s*\{\s*"([^"]+)"', f.read_text(errors="replace")))
    gui = root / "src/ui/GuiScenarioCases.cpp"
    if gui.exists():
        names.update(re.findall(r'"(gui\.[a-z0-9_.]+)"', gui.read_text(errors="replace")))
    return names


def runner_legs(root):
    legs = set()
    for f in (root / "scripts/regress").glob("*.sh"):
        legs.update(re.findall(r'regress_leg\s+"([^"]+)"', f.read_text(errors="replace")))
    return legs


def ctest_names(root):
    src = (root / "tests/CMakeLists.txt").read_text(errors="replace")
    return set(re.findall(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_.-]+)", src))


def self_test():
    """Build small fixture trees and hold the checker to each rule."""
    import subprocess
    import tempfile

    def tree(tmp, manual, table, tests="TEST_CASE (\"real case\") {}\n"):
        root = Path(tmp)
        (root / "tests").mkdir(parents=True, exist_ok=True)
        (root / "src/engine/scenario/cases").mkdir(parents=True, exist_ok=True)
        (root / "scripts/regress").mkdir(parents=True, exist_ok=True)
        (root / "MANUAL.md").write_text(manual)
        (root / "tests/manual_coverage.tsv").write_text(table)
        (root / "tests/a.cpp").write_text(tests)
        (root / "tests/CMakeLists.txt").write_text("add_test(NAME some-check COMMAND x)\n")
        return root

    manual = "---\ntitle: x\n---\n# Chap\n## One\n```\n# not a chapter\n```\n# Solo\ntext\n"
    head = "# ceiling gap 1\n# ceiling partial 0\n"
    good = head + "Chap > One\tb\tauto\ttest:real case\nSolo\tb\tgap\n"
    cases = [
        ("a complete table passes", good, 0),
        ("a heading with no row fails", head + "Chap > One\tb\tauto\ttest:real case\n", 1),
        ("a row for a missing heading fails", good + "Chap > Two\tb\tn/a\tnote\n", 1),
        ("unknown test evidence fails", head + "Chap > One\tb\tauto\ttest:no such case\nSolo\tb\tgap\n", 1),
        ("auto without evidence fails", head + "Chap > One\tb\tauto\nSolo\tb\tgap\n", 1),
        ("manual without a note fails",
         head + "Chap > One\tb\tmanual\nSolo\tb\tgap\n", 1),
        ("a gap above the ceiling fails",
         head + "Chap > One\tb\tgap\nSolo\tb\tgap\n", 1),
        ("a gap below the ceiling asks to lower it",
         head + "Chap > One\tb\tauto\ttest:real case\nSolo\tb\tauto\tctest:some-check\n", 1),
    ]
    failures = []
    for label, table, want in cases:
        with tempfile.TemporaryDirectory() as tmp:
            root = tree(tmp, manual, table)
            got = subprocess.run([sys.executable, __file__, str(root)],
                                 capture_output=True).returncode
            if (got == 0) != (want == 0):
                failures.append(f"{label}: exit {got}")
    if failures:
        fail(failures)
    print(f"manual coverage checker: {len(cases)} self-test cases pass")


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        self_test()
        return
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    root = Path(sys.argv[1]).resolve()
    manual = (root / "MANUAL.md").read_text(encoding="utf-8")
    table = root / "tests/manual_coverage.tsv"
    required, all_keys = manual_headings(manual)

    known = {
        "test": test_case_names(root),
        "scenario": scenario_names(root),
        "bb": runner_legs(root),
        "ctest": ctest_names(root),
    }

    errors, rows, ceilings = [], [], {}
    for n, raw in enumerate(table.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip():
            continue
        if raw.startswith("#"):
            m = re.match(r"#\s*ceiling\s+(gap|partial)\s+(0|[1-9][0-9]*)\s*$", raw)
            if m:
                ceilings[m.group(1)] = int(m.group(2))
            continue
        cols = [c.strip() for c in raw.split("\t")]
        if len(cols) < 3:
            errors.append(f"line {n}: needs at least section, behaviour and status")
            continue
        section, behaviour, status = cols[0], cols[1], cols[2]
        where = f"line {n} ({section} / {behaviour})"
        # After the status, a column made only of kind:name tokens is the
        # evidence and any other text is the note, so a row does not depend on
        # how many empty columns sit between them.
        evidence, note = "", ""
        for extra in (c for c in cols[3:] if c):
            tokens = [t.strip() for t in extra.split(";") if t.strip()]
            if tokens and all(t.partition(":")[0] in EVIDENCE_KINDS and t.partition(":")[2]
                              for t in tokens):
                if evidence:
                    errors.append(f"{where}: more than one evidence column")
                evidence = extra
            else:
                if note:
                    errors.append(f"{where}: more than one note column")
                note = extra
        if section not in all_keys:
            errors.append(f"{where}: no such manual heading")
        if not behaviour:
            errors.append(f"{where}: empty behaviour")
        if status not in STATUSES:
            errors.append(f"{where}: unknown status '{status}'")
            continue
        refs = [r.strip() for r in evidence.split(";") if r.strip()]
        if status in ("auto", "partial") and not refs:
            errors.append(f"{where}: '{status}' needs evidence")
        if status in ("gap", "manual", "n/a") and refs:
            errors.append(f"{where}: '{status}' carries evidence; use auto or partial")
        if status in ("partial", "manual", "n/a") and not note:
            errors.append(f"{where}: '{status}' needs a note saying why")
        for ref in refs:
            kind, _, name = ref.partition(":")
            if kind not in EVIDENCE_KINDS or not name:
                errors.append(f"{where}: evidence '{ref}' is not kind:name")
            elif kind == "script":
                if not (root / name).exists():
                    errors.append(f"{where}: script '{name}' does not exist")
            elif name not in known[kind]:
                errors.append(f"{where}: {kind} '{name}' does not exist")
        rows.append((section, status))

    covered = {s for s, _ in rows}
    for key in required:
        if key not in covered:
            errors.append(f"manual heading has no row: {key}")

    counts = {s: sum(1 for _, st in rows if st == s) for s in STATUSES}
    for kind in ("gap", "partial"):
        if kind not in ceilings:
            errors.append(f"missing '# ceiling {kind} N' line")
            continue
        if counts[kind] > ceilings[kind]:
            errors.append(f"{counts[kind]} {kind} rows exceed the ceiling of {ceilings[kind]}: "
                          "cover the behaviour, or raise the ceiling by hand and say why")
        elif counts[kind] < ceilings[kind]:
            errors.append(f"{kind} rows fell to {counts[kind]}: lower '# ceiling {kind}' "
                          f"from {ceilings[kind]} to {counts[kind]}")

    if errors:
        fail(errors)
    total = len(rows)
    print(f"manual coverage: {total} behaviours over {len(required)} headings - "
          + ", ".join(f"{counts[s]} {s}" for s in ("auto", "partial", "manual", "gap", "n/a")))


if __name__ == "__main__":
    main()
