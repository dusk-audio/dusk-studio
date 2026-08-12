#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <source-root> <python>" >&2
    exit 2
fi

SOURCE_ROOT="$1"
PYTHON="$2"
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-release-mechanics.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT

FIXTURE="$SCRATCH/repo"
mkdir -p "$FIXTURE/scripts" "$FIXTURE/packaging"
cp "$SOURCE_ROOT/scripts/bump-version.sh" "$FIXTURE/scripts/bump-version.sh"
printf '0.0.0\n' > "$FIXTURE/VERSION"
printf '## [9.9.9] - Unreleased\n' > "$FIXTURE/CHANGELOG.md"
printf '%s\n' \
    '<?xml version="1.0" encoding="UTF-8"?>' \
    '<component>' \
    '  <releases>' \
    '    <!-- scripts/bump-version.sh prepends new <release> entries here. -->' \
    '    <release version="0.0.0" date="2026-01-01">' \
    '      <description>' \
    '        <p>Decoy quoting the anchor:</p>' \
    '        <!-- scripts/bump-version.sh prepends new <release> entries here. -->' \
    '      </description>' \
    '    </release>' \
    '  </releases>' \
    '</component>' \
    > "$FIXTURE/packaging/DuskStudio.appdata.xml"
printf '%s\n' \
    '<!-- summary-start -->' \
    'stale summary that must not survive the bump' \
    '<!-- summary-end -->' \
    '' \
    '### Downloads' \
    '' \
    '- **Linux** (`.tar.xz`): unsigned.' \
    > "$FIXTURE/packaging/RELEASE-NOTES.md"

NOTES='Gain & grit <hot> > cool ]]> "quoted" café — release'
BUMP_OUTPUT="$SCRATCH/bump-output.txt"
(
    cd "$FIXTURE"
    bash scripts/bump-version.sh 9.9.9 "$NOTES" > "$BUMP_OUTPUT"
)

if command -v xmllint >/dev/null 2>&1; then
    xmllint --noout "$FIXTURE/packaging/DuskStudio.appdata.xml"

    # The abort path exists only when xmllint is present: a note carrying an
    # XML-illegal control character must fail the bump and leave both outputs
    # untouched.
    FIXTURE_BAD="$SCRATCH/repo-bad"
    mkdir -p "$FIXTURE_BAD/scripts" "$FIXTURE_BAD/packaging"
    cp "$SOURCE_ROOT/scripts/bump-version.sh" "$FIXTURE_BAD/scripts/bump-version.sh"
    printf '0.0.0\n' > "$FIXTURE_BAD/VERSION"
    printf '## [9.9.9] - Unreleased\n' > "$FIXTURE_BAD/CHANGELOG.md"
    printf '%s\n' \
        '<?xml version="1.0" encoding="UTF-8"?>' \
        '<component>' \
        '  <releases>' \
        '    <!-- scripts/bump-version.sh prepends new <release> entries here. -->' \
        '  </releases>' \
        '</component>' \
        > "$FIXTURE_BAD/packaging/DuskStudio.appdata.xml"
    printf '%s\n' \
        '<!-- summary-start -->' \
        'stale summary that must not survive the bump' \
        '<!-- summary-end -->' \
        > "$FIXTURE_BAD/packaging/RELEASE-NOTES.md"
    BAD_NOTES=$(printf 'bad\013note')
    if (cd "$FIXTURE_BAD" && bash scripts/bump-version.sh 9.9.9 "$BAD_NOTES" >/dev/null 2>&1); then
        echo "FAIL: control-character note must abort the bump" >&2
        exit 1
    fi
    if [[ "$(cat "$FIXTURE_BAD/VERSION")" != "0.0.0" ]]; then
        echo "FAIL: aborted bump must leave VERSION untouched" >&2
        exit 1
    fi
    if grep -q 'version="9.9.9"' "$FIXTURE_BAD/packaging/DuskStudio.appdata.xml"; then
        echo "FAIL: aborted bump must leave the appdata untouched" >&2
        exit 1
    fi
    if ! grep -q 'stale summary' "$FIXTURE_BAD/packaging/RELEASE-NOTES.md"; then
        echo "FAIL: aborted bump must leave the release notes untouched" >&2
        exit 1
    fi
fi

if [[ "$(cat "$FIXTURE/VERSION")" != "9.9.9" ]]; then
    echo "FAIL: bump-version.sh must write VERSION" >&2
    exit 1
fi

"$PYTHON" - "$FIXTURE/packaging/DuskStudio.appdata.xml" "$NOTES" "$SOURCE_ROOT" \
    "$FIXTURE/packaging/RELEASE-NOTES.md" "$BUMP_OUTPUT" <<'PY'
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

if not __debug__:
    raise SystemExit("refusing to run with assertions disabled")

appdata_path = Path(sys.argv[1])
expected_notes = sys.argv[2]
source_root = Path(sys.argv[3])
bumped_notes_path = Path(sys.argv[4])
bump_output_path = Path(sys.argv[5])

SUMMARY_START = "<!-- summary-start -->"
SUMMARY_END = "<!-- summary-end -->"


def summary_slot(text):
    before, start, rest = text.partition(SUMMARY_START)
    assert start, "release notes must carry the summary start marker"
    slot, end, after = rest.partition(SUMMARY_END)
    assert end, "release notes must carry the summary end marker"
    return before, slot.strip("\n"), after

root = ET.parse(appdata_path).getroot()
releases = root.findall("./releases/release")
assert len(releases) == 2, "expected the new entry plus the fixture's existing one"
release = releases[0]
assert release.get("version") == "9.9.9", "new entry must prepend, not append"
assert releases[1].get("version") == "0.0.0"
paragraph = release.find("./description/p")
assert paragraph is not None
assert paragraph.text == expected_notes, (paragraph.text, expected_notes)

workflows = {
    "manual-pdf.yml": "Manual PDF",
    "macos-release.yml": "macOS release (unsigned DMG)",
    "windows-build.yml": "Windows build",
    "linux-release.yml": "Linux release (tarball)",
}
create_marker = 'if ! create_err=$(gh release create "$TAG"'
recheck_marker = 'if gh release view "$TAG" --repo "$RELEASES_REPO" >/dev/null 2>&1; then'
upload_line = 'gh release upload "$TAG" --repo "$RELEASES_REPO" --clobber "${ASSETS[@]}"'
notes_marker = "--notes-file packaging/RELEASE-NOTES.md"

# All four workflows race to create the same release, so the body must come
# from one file in the tagged tree rather than from whichever job wins.
notes_path = source_root / "packaging" / "RELEASE-NOTES.md"
assert notes_path.is_file(), "packaging/RELEASE-NOTES.md must exist"
notes_text = notes_path.read_text(encoding="utf-8")
summary_slot(notes_text)  # markers present, else the bump silently skips it
visible_notes = re.sub(r"<!--.*?-->", "", notes_text, flags=re.S).strip()
assert visible_notes, "release notes must carry a body, not just comments"

# The summary is script-managed: the bump replaces the whole slot with the
# release notes verbatim and touches nothing outside it.
bumped_text = bumped_notes_path.read_text(encoding="utf-8")
before, slot, after = summary_slot(bumped_text)
assert slot == expected_notes, (slot, expected_notes)
assert before == "", "nothing may precede the summary slot"
assert "stale summary" not in bumped_text, "the old summary must be gone"
assert "### Downloads" in after, "the Downloads section must survive the bump"

# The printed handoff is the maintainer's release-day checklist. Pin its
# load-bearing order and CI-only packaging path.
bump_output = bump_output_path.read_text(encoding="utf-8")
checklist = (
    "5) Commit metadata:",
    "6) Land on main:",
    "7) Tag landed commit:",
    "8) Push tag:",
    "9) Wait for CI assets:",
    "10) Verify assets:",
)
for item in checklist:
    assert item in bump_output, f"release checklist is missing: {item}"
positions = [bump_output.index(item) for item in checklist]
assert positions == sorted(positions), "release checklist steps are out of order"
lines = {line.strip().split(")", 1)[0]: line for line in bump_output.splitlines()}
assert 'RELEASE_COMMIT=$(git rev-parse HEAD)' in lines["5"]
assert "land the recorded commit on origin/main" in lines["6"]
assert "git fetch origin" in lines["6"]
guarded_commit = '"${RELEASE_COMMIT:?run step 5 first}"'
assert f"git merge-base --is-ancestor {guarded_commit} origin/main" in lines["6"]
assert f'git tag -a v9.9.9 -m "Dusk Studio 9.9.9" {guarded_commit}' in lines["7"]
assert "git push origin v9.9.9" in lines["8"]
for display_name in workflows.values():
    assert display_name in lines["9"]
verifier_text = (source_root / "scripts" / "verify-release-assets.sh").read_text(
    encoding="utf-8"
)
expected_assets = re.findall(r'^\s+"[^"\n]+\|[^"\n]+"$', verifier_text, re.MULTILINE)
assert len(expected_assets) == 10, "release verifier must define ten assets"
assert f"all {len(expected_assets)} assets" in lines["9"]
assert "scripts/verify-release-assets.sh v9.9.9" in lines["10"]
for local_packager in (
    "package-tarball.sh",
    "package-macos.sh",
    "package-windows.ps1",
):
    assert local_packager not in bump_output

for name, display_name in workflows.items():
    text = (source_root / ".github" / "workflows" / name).read_text(encoding="utf-8")
    workflow_name = re.search(r"^name:\s*(.+)$", text, re.MULTILINE)
    assert workflow_name and workflow_name.group(1).strip() == display_name, (
        f"{name}: printed checklist name is stale"
    )
    assert text.count(create_marker) == 1, f"{name}: missing race-checked create"
    create_at = text.index(create_marker)
    create_end = text.index("2>&1); then", create_at)
    create_block = text[create_at:create_end]
    assert '"${ASSETS[@]}"' not in create_block, (
        f"{name}: create must not own asset upload"
    )
    assert notes_marker in create_block, (
        f"{name}: create must publish the canonical release notes file"
    )
    # Only the gh release commands are banned from carrying an inline body;
    # prose and other steps may say "--notes". Backslash continuations are
    # folded first so a multi-line invocation counts as one line.
    folded = re.sub(r"\\\n\s*", " ", text)
    for line in folded.splitlines():
        assert not ("gh release" in line and '--notes "' in line), (
            f"{name}: release body must not be composed per workflow"
        )
    recheck_at = text.index(recheck_marker, create_end)
    upload_matches = list(re.finditer(
        rf"^\s+{re.escape(upload_line)}$", text, re.MULTILINE
    ))
    assert len(upload_matches) == 1, f"{name}: upload must be unconditional"
    assert create_at < recheck_at < upload_matches[0].start()
PY
