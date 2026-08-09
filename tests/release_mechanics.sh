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

NOTES='Gain & grit <hot> > cool ]]> "quoted" café — release'
(
    cd "$FIXTURE"
    bash scripts/bump-version.sh 9.9.9 "$NOTES" >/dev/null
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
fi

if [[ "$(cat "$FIXTURE/VERSION")" != "9.9.9" ]]; then
    echo "FAIL: bump-version.sh must write VERSION" >&2
    exit 1
fi

"$PYTHON" - "$FIXTURE/packaging/DuskStudio.appdata.xml" "$NOTES" "$SOURCE_ROOT" <<'PY'
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

if not __debug__:
    raise SystemExit("refusing to run with assertions disabled")

appdata_path = Path(sys.argv[1])
expected_notes = sys.argv[2]
source_root = Path(sys.argv[3])

root = ET.parse(appdata_path).getroot()
releases = root.findall("./releases/release")
assert len(releases) == 2, "expected the new entry plus the fixture's existing one"
release = releases[0]
assert release.get("version") == "9.9.9", "new entry must prepend, not append"
assert releases[1].get("version") == "0.0.0"
paragraph = release.find("./description/p")
assert paragraph is not None
assert paragraph.text == expected_notes, (paragraph.text, expected_notes)

workflows = (
    "manual-pdf.yml",
    "macos-release.yml",
    "windows-build.yml",
    "linux-release.yml",
)
create_marker = 'if ! create_err=$(gh release create "$TAG"'
recheck_marker = 'if gh release view "$TAG" --repo "$RELEASES_REPO" >/dev/null 2>&1; then'
upload_line = 'gh release upload "$TAG" --repo "$RELEASES_REPO" --clobber "${ASSETS[@]}"'

for name in workflows:
    text = (source_root / ".github" / "workflows" / name).read_text(encoding="utf-8")
    assert text.count(create_marker) == 1, f"{name}: missing race-checked create"
    create_at = text.index(create_marker)
    create_end = text.index("2>&1); then", create_at)
    assert '"${ASSETS[@]}"' not in text[create_at:create_end], (
        f"{name}: create must not own asset upload"
    )
    recheck_at = text.index(recheck_marker, create_end)
    upload_matches = list(re.finditer(
        rf"^\s+{re.escape(upload_line)}$", text, re.MULTILINE
    ))
    assert len(upload_matches) == 1, f"{name}: upload must be unconditional"
    assert create_at < recheck_at < upload_matches[0].start()
PY
