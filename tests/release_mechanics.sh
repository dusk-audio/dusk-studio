#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <source-root> <python>" >&2
    exit 2
fi

SOURCE_ROOT="$1"
PYTHON="$2"
bash -n "$SOURCE_ROOT/scripts/run-selftest-xvfb.sh"
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/duskstudio-release-mechanics.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT

# The canonical helper must remain usable by dev.sh on macOS, where neither
# Xvfb nor GNU timeout ships. Simulate Darwin and require the direct path to
# carry the self-test flag to the selected binary.
DARWIN_BIN="$SCRATCH/darwin-bin"
mkdir -p "$DARWIN_BIN"
printf '%s\n' '#!/usr/bin/env bash' 'printf Darwin' > "$DARWIN_BIN/uname"
# Expand the fixture variable when the generated stub runs, not while writing it.
# shellcheck disable=SC2016
printf '%s\n' \
    '#!/usr/bin/env bash' \
    '[[ "${DUSKSTUDIO_RUN_SELFTEST:-}" == 1 ]]' \
    > "$DARWIN_BIN/selftest"
chmod +x "$DARWIN_BIN/uname" "$DARWIN_BIN/selftest"
PATH="$DARWIN_BIN:$PATH" \
    "$SOURCE_ROOT/scripts/run-selftest-xvfb.sh" "$DARWIN_BIN/selftest"

# A changelog mention is not a release heading. Refuse it before any metadata
# can be touched, and make the diagnostic identify the exact required form.
FIXTURE_BAD_HEADING="$SCRATCH/repo-bad-heading"
mkdir -p "$FIXTURE_BAD_HEADING/scripts"
cp "$SOURCE_ROOT/scripts/bump-version.sh" \
    "$FIXTURE_BAD_HEADING/scripts/bump-version.sh"
printf '0.0.0\n' > "$FIXTURE_BAD_HEADING/VERSION"
printf 'Notes mention ## [9.9.9] but this is not a heading.\n' \
    > "$FIXTURE_BAD_HEADING/CHANGELOG.md"
BAD_HEADING_ERROR="$SCRATCH/bad-heading-error.txt"
if (cd "$FIXTURE_BAD_HEADING" \
    && bash scripts/bump-version.sh 9.9.9 test \
        >/dev/null 2>"$BAD_HEADING_ERROR"); then
    echo "FAIL: a changelog mention must not satisfy the release heading gate" >&2
    exit 1
fi
if ! grep -qF "no exact '## [9.9.9] - Unreleased' heading" \
    "$BAD_HEADING_ERROR"; then
    echo "FAIL: malformed changelog heading reported the wrong failure" >&2
    cat "$BAD_HEADING_ERROR" >&2
    exit 1
fi
if [[ "$(cat "$FIXTURE_BAD_HEADING/VERSION")" != "0.0.0" ]]; then
    echo "FAIL: a bad changelog heading must leave VERSION untouched" >&2
    exit 1
fi

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

# Missing release metadata must abort atomically instead of returning success
# after writing only VERSION.
FIXTURE_MISSING="$SCRATCH/repo-missing-appdata"
mkdir -p "$FIXTURE_MISSING/scripts" "$FIXTURE_MISSING/packaging"
cp "$SOURCE_ROOT/scripts/bump-version.sh" "$FIXTURE_MISSING/scripts/bump-version.sh"
printf '0.0.0\n' > "$FIXTURE_MISSING/VERSION"
printf '## [9.9.9] - Unreleased\n' > "$FIXTURE_MISSING/CHANGELOG.md"
printf '%s\n' \
    '<!-- summary-start -->' \
    'old summary' \
    '<!-- summary-end -->' \
    > "$FIXTURE_MISSING/packaging/RELEASE-NOTES.md"
MISSING_APPDATA_ERROR="$SCRATCH/missing-appdata-error.txt"
if (cd "$FIXTURE_MISSING" \
    && bash scripts/bump-version.sh 9.9.9 test \
        >/dev/null 2>"$MISSING_APPDATA_ERROR"); then
    echo "FAIL: missing AppStream metadata must abort the bump" >&2
    exit 1
fi
if ! grep -qF 'error: packaging/DuskStudio.appdata.xml not found' \
    "$MISSING_APPDATA_ERROR"; then
    echo "FAIL: missing AppStream metadata reported the wrong failure" >&2
    cat "$MISSING_APPDATA_ERROR" >&2
    exit 1
fi
if [[ "$(cat "$FIXTURE_MISSING/VERSION")" != "0.0.0" ]]; then
    echo "FAIL: a missing AppStream file must leave VERSION untouched" >&2
    exit 1
fi
if ! grep -q 'old summary' "$FIXTURE_MISSING/packaging/RELEASE-NOTES.md"; then
    echo "FAIL: an aborted bump must leave release notes untouched" >&2
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

# The release-day self-test must be isolated from the maintainer's live
# Wayland session and must fail closed if its private X server cannot start.
selftest_helper = (source_root / "scripts" / "run-selftest-xvfb.sh").read_text(
    encoding="utf-8"
)
for required in (
    "set -euo pipefail",
    '"$(uname -s)" == "Darwin"',
    "exec env DUSKSTUDIO_RUN_SELFTEST=1",
    "-displayfd",
    "-nolisten tcp",
    "trap cleanup EXIT",
    "trap 'exit 130' INT",
    "trap 'exit 143' TERM",
    "env -u WAYLAND_DISPLAY",
    'DISPLAY="$DISPLAY_NUM"',
    'DUSKSTUDIO_SELFTEST_TIMEOUT:-90s',
    'DUSKSTUDIO_SELFTEST_KILL_AFTER:-10s',
    'DUSKSTUDIO_XVFB_START_ATTEMPTS:-100',
    'timeout --kill-after="$SELFTEST_KILL_AFTER" "$SELFTEST_TIMEOUT"',
):
    assert required in selftest_helper, f"self-test helper lost safety guard: {required}"
assert "xvfb-run" not in selftest_helper, (
    "self-test helper must work on the maintainer host, which has Xvfb but no xvfb-run"
)
safe_selftest_callers = {
    "maintainer guide": source_root / "docs" / "MAINTAINER-GUIDE.md",
    "Linux build guide": source_root / "BUILDING-LINUX.md",
    "developer helper": source_root / "scripts" / "dev.sh",
    "agent instructions": source_root / "CLAUDE.md",
}
for label, path in safe_selftest_callers.items():
    caller = path.read_text(encoding="utf-8")
    assert "scripts/run-selftest-xvfb.sh" in caller, (
        f"{label} must route self-tests through the private-Xvfb helper"
    )
    assert "DUSKSTUDIO_RUN_SELFTEST=1 ./build" not in caller, (
        f"{label} must not launch a self-test on the live display"
    )

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
    "4) Rebuild + smoke:",
    "5) Commit metadata:",
    "6) Fetch main:",
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
assert "scripts/run-selftest-xvfb.sh" in lines["4"]
assert "DUSKSTUDIO_RUN_SELFTEST" not in bump_output
assert "git fetch origin main" in lines["6"]
assert 'echo "STOP: git fetch origin main failed" >&2; false' in lines["6"]
guarded_commit = '"${RELEASE_COMMIT:?record RELEASE_COMMIT after committing metadata}"'
landing_line = next(
    line for line in bump_output.splitlines() if "Prove landing:" in line
)
assert f"git merge-base --is-ancestor {guarded_commit} origin/main" in landing_line
assert '&& echo "landed on origin/main"' in landing_line
assert 'echo "STOP: release commit is not on origin/main" >&2; false' in landing_line
assert "PR squash:" in bump_output
assert "re-record RELEASE_COMMIT as the landed commit" in bump_output
assert f'git tag -a v9.9.9 -m "Dusk Studio 9.9.9" {guarded_commit}' in lines["7"]
assert 'git push origin "refs/tags/v9.9.9"' in lines["8"]
for display_name in workflows.values():
    assert display_name in lines["9"]
verifier_text = (source_root / "scripts" / "verify-release-assets.sh").read_text(
    encoding="utf-8"
)
expected_assets = re.findall(r'^\s+"[^"\n]+\|[^"\n]+"$', verifier_text, re.MULTILINE)
expected_asset_count = 10
assert len(expected_assets) == expected_asset_count, (
    "release verifier asset count changed; update the release contract explicitly"
)
assert f"all {expected_asset_count} assets" in lines["9"], (
    "bump-version.sh prints a stale release asset count"
)
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
