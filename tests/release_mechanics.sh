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
# Keep the fixture's Markdown backticks literal.
# shellcheck disable=SC2016
printf '%s\n' \
    '<!-- summary-start -->' \
    'stale summary that must not survive the bump' \
    '<!-- summary-end -->' \
    '' \
    '### Downloads' \
    '' \
    '- **Linux** (`.tar.xz`): unsigned.' \
    > "$FIXTURE/packaging/RELEASE-NOTES.md"

WHITESPACE_ERROR="$SCRATCH/whitespace-notes-error.txt"
if (cd "$FIXTURE" \
    && bash scripts/bump-version.sh 9.9.9 '' \
        >/dev/null 2>"$WHITESPACE_ERROR"); then
    echo "FAIL: an explicitly empty release summary must be rejected" >&2
    exit 1
fi
if ! grep -qF 'release notes must contain visible text' \
    "$WHITESPACE_ERROR"; then
    echo "FAIL: an empty release summary reported the wrong failure" >&2
    cat "$WHITESPACE_ERROR" >&2
    exit 1
fi
if (cd "$FIXTURE" \
    && bash scripts/bump-version.sh 9.9.9 '   ' \
        >/dev/null 2>"$WHITESPACE_ERROR"); then
    echo "FAIL: whitespace-only release notes must be rejected" >&2
    exit 1
fi
if ! grep -qF 'release notes must contain visible text' \
    "$WHITESPACE_ERROR"; then
    echo "FAIL: whitespace-only release notes reported the wrong failure" >&2
    cat "$WHITESPACE_ERROR" >&2
    exit 1
fi
if [[ "$(cat "$FIXTURE/VERSION")" != "0.0.0" ]]; then
    echo "FAIL: whitespace-only release notes must leave VERSION untouched" >&2
    exit 1
fi
if (cd "$FIXTURE" \
    && bash scripts/bump-version.sh 9.9.9 '<!-- TODO: write summary -->' \
        >/dev/null 2>"$WHITESPACE_ERROR"); then
    echo "FAIL: comment-only release notes must be rejected before publishing" >&2
    exit 1
fi
if ! grep -qF 'release notes must contain visible text' "$WHITESPACE_ERROR"; then
    echo "FAIL: comment-only release notes reported the wrong failure" >&2
    cat "$WHITESPACE_ERROR" >&2
    exit 1
fi

# An anchor quoted inside a nested release description is a decoy, not the
# direct child insertion point. A file containing only that decoy must abort
# before any metadata lands.
FIXTURE_NESTED="$SCRATCH/repo-nested-anchor"
mkdir -p "$FIXTURE_NESTED/scripts" "$FIXTURE_NESTED/packaging"
cp "$SOURCE_ROOT/scripts/bump-version.sh" "$FIXTURE_NESTED/scripts/bump-version.sh"
printf '0.0.0\n' > "$FIXTURE_NESTED/VERSION"
printf '## [9.9.9] - Unreleased\n' > "$FIXTURE_NESTED/CHANGELOG.md"
printf '%s\n' \
    '<?xml version="1.0" encoding="UTF-8"?>' \
    '<component>' \
    '  <releases>' \
    '    <release version="0.0.0" date="2026-01-01">' \
    '      <description>' \
    '        <!-- scripts/bump-version.sh prepends new <release> entries here. -->' \
    '      </description>' \
    '    </release>' \
    '  </releases>' \
    '</component>' \
    > "$FIXTURE_NESTED/packaging/DuskStudio.appdata.xml"
printf '%s\n' \
    '<!-- summary-start -->' \
    'nested-anchor summary' \
    '<!-- summary-end -->' \
    > "$FIXTURE_NESTED/packaging/RELEASE-NOTES.md"
NESTED_ERROR="$SCRATCH/nested-anchor-error.txt"
if (cd "$FIXTURE_NESTED" \
    && bash scripts/bump-version.sh 9.9.9 test \
        >/dev/null 2>"$NESTED_ERROR"); then
    echo "FAIL: a nested AppStream anchor must not accept the release" >&2
    exit 1
fi
if ! grep -qF 'must contain exactly one direct release anchor' "$NESTED_ERROR"; then
    echo "FAIL: nested AppStream anchor reported the wrong failure" >&2
    cat "$NESTED_ERROR" >&2
    exit 1
fi
if [[ "$(cat "$FIXTURE_NESTED/VERSION")" != "0.0.0" ]] \
    || ! grep -qF 'nested-anchor summary' \
        "$FIXTURE_NESTED/packaging/RELEASE-NOTES.md" \
    || grep -q 'version="9.9.9"' \
        "$FIXTURE_NESTED/packaging/DuskStudio.appdata.xml"; then
    echo "FAIL: rejected nested anchor must leave all metadata untouched" >&2
    exit 1
fi

# Fail the third application move, after notes and AppStream have landed, and
# require the rollback to restore all three original files byte-for-byte.
FIXTURE_ROLLBACK="$SCRATCH/repo-rollback"
cp -R "$FIXTURE" "$FIXTURE_ROLLBACK"
chmod 640 "$FIXTURE_ROLLBACK/VERSION"
chmod 600 "$FIXTURE_ROLLBACK/packaging/RELEASE-NOTES.md"
chmod 620 "$FIXTURE_ROLLBACK/packaging/DuskStudio.appdata.xml"
ROLLBACK_EXPECTED="$SCRATCH/rollback-expected"
mkdir -p "$ROLLBACK_EXPECTED"
cp "$FIXTURE_ROLLBACK/VERSION" "$ROLLBACK_EXPECTED/VERSION"
cp "$FIXTURE_ROLLBACK/packaging/RELEASE-NOTES.md" \
    "$ROLLBACK_EXPECTED/RELEASE-NOTES.md"
cp "$FIXTURE_ROLLBACK/packaging/DuskStudio.appdata.xml" \
    "$ROLLBACK_EXPECTED/DuskStudio.appdata.xml"
MV_SHIM="$SCRATCH/mv-shim"
mkdir -p "$MV_SHIM"
# Expand these variables when the generated mv shim runs.
# shellcheck disable=SC2016
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'count=0' \
    '[[ -f "$DUSK_MV_COUNT" ]] && read -r count < "$DUSK_MV_COUNT"' \
    'count=$((count + 1))' \
    'printf "%s\n" "$count" > "$DUSK_MV_COUNT"' \
    'if [[ "$count" == "$DUSK_MV_FAIL_AT" ]]; then exit 73; fi' \
    'if [[ "$count" == "${DUSK_MV_SIGNAL_AT:-}" ]]; then' \
    '    "$DUSK_REAL_MV" "$@"' \
    '    kill -TERM "$PPID"' \
    '    exit 0' \
    'fi' \
    'exec "$DUSK_REAL_MV" "$@"' \
    > "$MV_SHIM/mv"
chmod +x "$MV_SHIM/mv"
ROLLBACK_ERROR="$SCRATCH/rollback-error.txt"
REAL_MV="$(command -v mv)"
if (cd "$FIXTURE_ROLLBACK" \
    && PATH="$MV_SHIM:$PATH" \
       DUSK_REAL_MV="$REAL_MV" \
       DUSK_MV_COUNT="$SCRATCH/mv-count" \
       DUSK_MV_FAIL_AT=3 \
       bash scripts/bump-version.sh 9.9.9 rollback \
        >/dev/null 2>"$ROLLBACK_ERROR"); then
    echo "FAIL: injected VERSION application failure must abort" >&2
    exit 1
fi
if ! grep -qF 'release metadata application failed; restoring originals' \
    "$ROLLBACK_ERROR"; then
    echo "FAIL: injected application failure did not enter rollback" >&2
    cat "$ROLLBACK_ERROR" >&2
    exit 1
fi
if [[ "$(cat "$SCRATCH/mv-count")" != 3 ]]; then
    echo "FAIL: rollback fixture did not fail on the third metadata move" >&2
    exit 1
fi
cmp "$ROLLBACK_EXPECTED/VERSION" "$FIXTURE_ROLLBACK/VERSION"
cmp "$ROLLBACK_EXPECTED/RELEASE-NOTES.md" \
    "$FIXTURE_ROLLBACK/packaging/RELEASE-NOTES.md"
cmp "$ROLLBACK_EXPECTED/DuskStudio.appdata.xml" \
    "$FIXTURE_ROLLBACK/packaging/DuskStudio.appdata.xml"
"$PYTHON" - "$FIXTURE_ROLLBACK" <<'PY'
import os
import stat
import sys

root = sys.argv[1]
expected = {
    "VERSION": 0o640,
    "packaging/RELEASE-NOTES.md": 0o600,
    "packaging/DuskStudio.appdata.xml": 0o620,
}
for relative, mode in expected.items():
    actual = stat.S_IMODE(os.stat(os.path.join(root, relative)).st_mode)
    if actual != mode:
        raise SystemExit(f"FAIL: rollback changed {relative} mode to {actual:o}")
PY
if find "$FIXTURE_ROLLBACK" -type f \
    \( -name '*.tmp' -o -name '*.rollback.*' \) -print -quit | grep -q .; then
    echo "FAIL: successful rollback left transaction files behind" >&2
    exit 1
fi

# The release body always has a static Downloads section, so byte length alone
# cannot prove that bump-version.sh populated the summary slot. Exercise the
# verifier with a fake gh response while keeping the complete asset set valid.
VERIFIER_BIN="$SCRATCH/verifier-bin"
mkdir -p "$VERIFIER_BIN"
cat > "$VERIFIER_BIN/gh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ -n "${FAKE_GH_STDERR:-}" ]]; then
    printf '%s\n' "$FAKE_GH_STDERR" >&2
fi
if [[ -n "${FAKE_GH_FAIL_JSON:-}" \
    && " $* " == *" --json ${FAKE_GH_FAIL_JSON} "* ]]; then
    printf 'fake gh %s failure\n' "$FAKE_GH_FAIL_JSON" >&2
    exit 73
fi
case " $* " in
    *" --json body "*)
        printf '%s\n' "${FAKE_RELEASE_BODY-}"
        ;;
    *" --json assets "*)
        if [[ ${FAKE_RELEASE_ASSETS+x} == x ]]; then
            printf '%s\n' "$FAKE_RELEASE_ASSETS"
        else
            printf '%s\n' \
                'dusk-studio-9.9.9-Linux-x86_64.tar.xz' \
                'dusk-studio-9.9.9-Linux-aarch64.tar.xz' \
                'dusk-studio-9.9.9-macOS-arm64.dmg' \
                'dusk-studio-9.9.9-Windows-x64.msi' \
                'MANUAL.pdf' \
                'SHA256SUMS'
        fi
        ;;
    *)
        echo "unexpected fake gh invocation: $*" >&2
        exit 2
        ;;
esac
SH
chmod +x "$VERIFIER_BIN/gh"

PLACEHOLDER_BODY=$(printf '%s\n' \
    '<!-- summary-start -->' \
    '<!-- summary-end -->' \
    '' \
    '### Downloads')
COMMENT_ONLY_BODY=$(printf '%s\n' \
    '<!-- summary-start -->' \
    '<!-- TODO: write the release summary -->' \
    '<!-- summary-end -->' \
    '' \
    '### Downloads')
INVALID_BODY='### Downloads'
INVALID_ORDER_BODY=$(printf '%s\n' \
    '<!-- summary-end -->' \
    '<!-- summary-start -->' \
    'Release summary for 9.9.9.' \
    '' \
    '### Downloads')
DUPLICATE_START_BODY=$(printf '%s\n' \
    '<!-- summary-start -->' \
    'Release summary for 9.9.9.' \
    '<!-- summary-start -->' \
    '<!-- summary-end -->' \
    '' \
    '### Downloads')
LEADING_END_BODY=$(printf '%s\n' \
    '<!-- summary-end -->' \
    '<!-- summary-start -->' \
    'Release summary for 9.9.9.' \
    '<!-- summary-end -->' \
    '' \
    '### Downloads')
TRUNCATED_BODY=$(printf '%s\n' \
    '<!-- summary-start -->' \
    'Release summary for 9.9.9.' \
    '' \
    '### Downloads')
POPULATED_BODY=$(printf '%s\n' \
    '<!-- summary-start -->' \
    'Release summary for 9.9.9.' \
    '<!-- summary-end -->' \
    '' \
    '### Downloads')
MISSING_ASSETS=$(printf '%s\n' \
    'dusk-studio-9.9.9-Linux-x86_64.tar.xz' \
    'dusk-studio-9.9.9-Linux-aarch64.tar.xz' \
    'dusk-studio-9.9.9-macOS-arm64.dmg' \
    'dusk-studio-9.9.9-Windows-x64.msi' \
    'SHA256SUMS')
VERIFIER_OUTPUT="$SCRATCH/verifier-output.txt"
VERIFIER_ERROR="$SCRATCH/verifier-error.txt"
if PATH="$VERIFIER_BIN:$PATH" FAKE_GH_FAIL_JSON=assets \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: an asset API failure must abort verification" >&2
    exit 1
fi
if ! grep -qF 'cannot read release v9.9.9' "$VERIFIER_OUTPUT" \
    || ! grep -qF 'fake gh assets failure' "$VERIFIER_OUTPUT"; then
    echo "FAIL: asset API failure diagnostics were lost" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_GH_FAIL_JSON=body \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: a body API failure must abort verification" >&2
    exit 1
fi
if ! grep -qF 'cannot read release body for v9.9.9' "$VERIFIER_OUTPUT" \
    || ! grep -qF 'fake gh body failure' "$VERIFIER_OUTPUT"; then
    echo "FAIL: body API failure diagnostics were lost" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_ASSETS='' \
    FAKE_RELEASE_BODY="$POPULATED_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: a release with no assets must fail verification" >&2
    exit 1
fi
if ! grep -qE '^MISSING[[:space:]]+' "$VERIFIER_OUTPUT"; then
    echo "FAIL: an empty asset list reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_ASSETS="$MISSING_ASSETS" \
    FAKE_RELEASE_BODY="$POPULATED_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: a release missing one asset must fail verification" >&2
    exit 1
fi
if ! grep -qE '^MISSING[[:space:]].*MANUAL\.pdf$' "$VERIFIER_OUTPUT"; then
    echo "FAIL: a missing release asset reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY='' \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: an empty release body must fail verification" >&2
    exit 1
fi
if ! grep -qE '^EMPTY[[:space:]]+release body' "$VERIFIER_OUTPUT"; then
    echo "FAIL: empty release body reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY="$PLACEHOLDER_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: an empty release-summary slot must fail verification" >&2
    exit 1
fi
if ! grep -qE '^PLACEHOLDER[[:space:]]+release body' "$VERIFIER_OUTPUT"; then
    echo "FAIL: empty release-summary slot reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY="$COMMENT_ONLY_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: a comment-only release-summary slot must fail verification" >&2
    exit 1
fi
if ! grep -qE '^PLACEHOLDER[[:space:]]+release body' "$VERIFIER_OUTPUT"; then
    echo "FAIL: comment-only release summary reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY="$INVALID_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: missing release-summary markers must fail verification" >&2
    exit 1
fi
if ! grep -qE '^INVALID[[:space:]]+release body' "$VERIFIER_OUTPUT"; then
    echo "FAIL: missing release-summary markers reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY="$INVALID_ORDER_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: out-of-order release-summary markers must fail verification" >&2
    exit 1
fi
if ! grep -qE '^INVALID[[:space:]]+release body' "$VERIFIER_OUTPUT"; then
    echo "FAIL: out-of-order release-summary markers reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY="$DUPLICATE_START_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: duplicate release-summary start markers must fail verification" >&2
    exit 1
fi
if ! grep -qE '^INVALID[[:space:]]+release body' "$VERIFIER_OUTPUT"; then
    echo "FAIL: duplicate release-summary start markers reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY="$LEADING_END_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: duplicate release-summary end markers must fail verification" >&2
    exit 1
fi
if ! grep -qE '^INVALID[[:space:]]+release body' "$VERIFIER_OUTPUT"; then
    echo "FAIL: duplicate release-summary end markers reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY="$TRUNCATED_BODY" \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>&1; then
    echo "FAIL: a release-summary slot without an end marker must fail verification" >&2
    exit 1
fi
if ! grep -qE '^INVALID[[:space:]]+release body' "$VERIFIER_OUTPUT"; then
    echo "FAIL: truncated release-summary slot reported the wrong failure" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
PATH="$VERIFIER_BIN:$PATH" FAKE_RELEASE_BODY="$POPULATED_BODY" \
    FAKE_GH_STDERR='benign gh diagnostic' \
    bash "$SOURCE_ROOT/scripts/verify-release-assets.sh" v9.9.9 \
    >"$VERIFIER_OUTPUT" 2>"$VERIFIER_ERROR"
if ! grep -qF "summary populated" "$VERIFIER_OUTPUT"; then
    echo "FAIL: populated release summary was not accepted" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi
if grep -qF 'EXTRA' "$VERIFIER_OUTPUT"; then
    echo "FAIL: gh stderr must not be parsed as a release asset" >&2
    cat "$VERIFIER_OUTPUT" >&2
    exit 1
fi

# An interrupt after a metadata move must use the same rollback path instead
# of letting the EXIT cleanup discard the recovery copies.
FIXTURE_SIGNAL="$SCRATCH/repo-signal"
cp -R "$FIXTURE" "$FIXTURE_SIGNAL"
SIGNAL_ERROR="$SCRATCH/signal-error.txt"
if (cd "$FIXTURE_SIGNAL" \
    && PATH="$MV_SHIM:$PATH" \
       DUSK_REAL_MV="$REAL_MV" \
       DUSK_MV_COUNT="$SCRATCH/mv-count-signal" \
       DUSK_MV_FAIL_AT=0 \
       DUSK_MV_SIGNAL_AT=2 \
       bash scripts/bump-version.sh 9.9.9 interrupted \
        >/dev/null 2>"$SIGNAL_ERROR"); then
    echo "FAIL: an interrupt during metadata application must abort" >&2
    exit 1
fi
if ! grep -qF 'application interrupted by TERM; restoring originals' \
    "$SIGNAL_ERROR"; then
    echo "FAIL: interrupted application did not enter rollback" >&2
    cat "$SIGNAL_ERROR" >&2
    exit 1
fi
if [[ "$(cat "$SCRATCH/mv-count-signal")" != 2 ]]; then
    echo "FAIL: signal fixture did not interrupt after the second move" >&2
    exit 1
fi
cmp "$FIXTURE/VERSION" "$FIXTURE_SIGNAL/VERSION"
cmp "$FIXTURE/packaging/RELEASE-NOTES.md" \
    "$FIXTURE_SIGNAL/packaging/RELEASE-NOTES.md"
cmp "$FIXTURE/packaging/DuskStudio.appdata.xml" \
    "$FIXTURE_SIGNAL/packaging/DuskStudio.appdata.xml"
if find "$FIXTURE_SIGNAL" -type f \
    \( -name '*.tmp' -o -name '*.rollback.*' \) -print -quit | grep -q .; then
    echo "FAIL: interrupted rollback left transaction files behind" >&2
    exit 1
fi

# VERSION is staged before either metadata move. If that staging write fails,
# all originals must remain untouched and no rollback should be necessary.
FIXTURE_VERSION_STAGE="$SCRATCH/repo-version-stage"
cp -R "$FIXTURE" "$FIXTURE_VERSION_STAGE"
mkdir "$FIXTURE_VERSION_STAGE/VERSION.tmp"
if (cd "$FIXTURE_VERSION_STAGE" \
    && bash scripts/bump-version.sh 9.9.9 stage-failure \
        >/dev/null 2>&1); then
    echo "FAIL: an unwritable VERSION.tmp must abort" >&2
    exit 1
fi
cmp "$FIXTURE/VERSION" "$FIXTURE_VERSION_STAGE/VERSION"
cmp "$FIXTURE/packaging/RELEASE-NOTES.md" \
    "$FIXTURE_VERSION_STAGE/packaging/RELEASE-NOTES.md"
cmp "$FIXTURE/packaging/DuskStudio.appdata.xml" \
    "$FIXTURE_VERSION_STAGE/packaging/DuskStudio.appdata.xml"

NOTES='Gain & grit <hot> > cool ]]> "quoted" café — release'
RAW_NOTES="$NOTES <!-- internal release-note comment -->"
BUMP_OUTPUT="$SCRATCH/bump-output.txt"
(
    cd "$FIXTURE"
    bash scripts/bump-version.sh 9.9.9 "$RAW_NOTES" > "$BUMP_OUTPUT"
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
import subprocess
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

sfizz_tree = subprocess.run(
    ["git", "-C", str(source_root), "ls-tree", "HEAD", "external/sfizz"],
    check=True,
    capture_output=True,
    text=True,
).stdout.strip()
sfizz_tree_entry = re.fullmatch(
    r"160000 commit ([0-9a-f]{40})\texternal/sfizz", sfizz_tree
)
assert sfizz_tree_entry, "external/sfizz must be a pinned git submodule"
sfizz_revision = sfizz_tree_entry.group(1)

licenses = (source_root / "LICENSES.txt").read_text(encoding="utf-8")
sfizz_header_revision = re.search(
    r"dusk-fizz \(SFZ.*?Version\s+:.*?submodule rev\s+([0-9a-f]{40})",
    licenses,
    re.DOTALL,
)
sfizz_license_revision = re.search(
    r"external/sfizz/LICENSE,\s+submodule rev\s+([0-9a-f]{40})",
    licenses,
)
assert sfizz_header_revision and sfizz_license_revision, (
    "LICENSES.txt must record the dusk-fizz and sfizz license revisions"
)
recorded_sfizz_revisions = [
    sfizz_header_revision.group(1),
    sfizz_license_revision.group(1),
    *re.findall(
        r"\bsfizz(?:\s+submodule)?\s+rev\s+([0-9a-f]{40})", licenses
    ),
]
assert len(recorded_sfizz_revisions) == 15, (
    "LICENSES.txt sfizz revision inventory changed without updating the "
    "release contract"
)
assert set(recorded_sfizz_revisions) == {sfizz_revision}, (
    "LICENSES.txt sfizz revisions must match the external/sfizz gitlink: "
    f"expected {sfizz_revision}, found {sorted(set(recorded_sfizz_revisions))}"
)


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
release_date = release.get("date")
assert release_date and re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", release_date)
assert releases[1].get("version") == "0.0.0"
paragraph = release.find("./description/p")
assert paragraph is not None
assert paragraph.text == expected_notes, (paragraph.text, expected_notes)

workflows = {
    "release.yml": "Dusk Studio release",
}
notes_marker = "--notes-file packaging/RELEASE-NOTES.md"
preflight_marker = "- name: Preflight - releases-repo token is valid (tag builds only)"
action_pins = {
    "actions/checkout": ("3d3c42e5aac5ba805825da76410c181273ba90b1", 6),
    "actions/upload-artifact": ("043fb46d1a93c77aae656e7c1c64a875d1fc6a0a", 4),
    "actions/download-artifact": ("37930b1c2abaa49bbe596cd826c3c89aef350131", 1),
    "actions/cache": ("55cc8345863c7cc4c66a329aec7e433d2d1c52a9", 1),
}

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
maintainer_guide = safe_selftest_callers["maintainer guide"].read_text(encoding="utf-8")
release_workflow = (source_root / ".github" / "workflows" / "release.yml").read_text(
    encoding="utf-8"
)
pinned_donor = re.search(
    r"^\s*DONOR_REV:\s*([0-9a-f]{40})$", release_workflow, re.MULTILINE
)
assert pinned_donor, "release.yml must pin DONOR_REV"
donor_pins = {}
for workflow_path in (source_root / ".github" / "workflows").glob("*.yml"):
    pins = re.findall(
        r"^\s*DONOR_REV:\s*([0-9a-f]{40})$",
        workflow_path.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if pins:
        donor_pins[workflow_path.name] = pins
expected_donor_workflows = {
    "linux-build.yml",
    "linux-sanitizer.yml",
    "macos-build.yml",
    "raspberry-pi-build.yml",
    "release.yml",
    "windows-tests.yml",
}
assert set(donor_pins) == expected_donor_workflows, (
    "exactly the release and test workflows that consume donor source must "
    f"pin DONOR_REV: {donor_pins.keys()}"
)
assert all(
    pins and set(pins) == {pinned_donor.group(1)}
    for pins in donor_pins.values()
), (
    f"DONOR_REV drift across workflows: {donor_pins}"
)

windows_pr_workflow = (
    source_root / ".github" / "workflows" / "windows-tests.yml"
).read_text(encoding="utf-8")
windows_pr_build = re.search(
    r"(?ms)^      - name: Build app and test binary\n(?P<body>.*?)(?=^      - name: |\Z)",
    windows_pr_workflow,
)
assert windows_pr_build, "windows-tests.yml lost its app + test build step"
assert re.search(
    r"cmake --build build-tests --config Release --target "
    r"DuskStudio dusk-studio-tests -j4",
    windows_pr_build.group("body"),
), "Windows PR CI must compile both the app and test targets"

file_importer_tests = (
    "FileImporter: transient file locks retry but remain bounded",
    "FileImporter: 44.1k mono -> 48k session preserves length",
    "FileImporter: 44.1k -> 48k upsample fills the tail with audio, not a held sample",
    "FileImporter: 96k mono -> 48k session preserves length",
    "FileImporter: stereo -> mono sums L+R at 0.5 each",
    "FileImporter: mono -> stereo duplicates to L and R",
    "FileImporter: matching rate and channels copies the source verbatim",
    "FileImporter: a matching 32-bit source is not truncated to 24-bit",
)
for workflow_name in ("windows-tests.yml", "release.yml"):
    workflow_text = (
        source_root / ".github" / "workflows" / workflow_name
    ).read_text(encoding="utf-8")
    folded_workflow = re.sub(r"\\\n\s*", " ", workflow_text)
    assert not re.search(
        r"ctest[^\n]*(?:-E|--exclude-regex)[^\n]*FileImporter",
        folded_workflow,
    ), (
        f"{workflow_name} must not exclude the FileImporter suite"
    )
    for test_name in file_importer_tests:
        assert test_name in workflow_text, (
            f"{workflow_name} lost required FileImporter inventory: {test_name}"
        )

sanitizer_workflow = (
    source_root / ".github" / "workflows" / "linux-sanitizer.yml"
).read_text(encoding="utf-8")
asan_job_match = re.search(
    r"(?ms)^  asan-ubsan-tests:\n(?P<body>.*?)(?=^  [a-z0-9-]+:\n|\Z)",
    sanitizer_workflow,
)
assert asan_job_match, "linux-sanitizer.yml lost its asan-ubsan-tests job"
asan_job = asan_job_match.group("body")
intentional_leak_tests = (
    "NativeInsertSlot bumps its generation on every identity change",
    "AlsaAudioIODevice: wedged I/O thread leaks the device, never frees it",
    "TapeMachineDSP nulls against the JUCE donor at 1x",
    "TapeMachineDSP tracks the JUCE donor within tolerance at 2x and 4x",
    "TapeMachineDSP latency is stable per oversampling factor",
)
for required in (
    "DUSKSTUDIO_ENABLE_ASAN=ON",
    "ASAN_SYMBOLIZER_PATH: /usr/bin/llvm-symbolizer",
    "halt_on_error=1:abort_on_error=1:detect_leaks=1:symbolize=1",
    "halt_on_error=1:abort_on_error=1:print_stacktrace=1",
):
    assert required in asan_job, (
        f"linux-sanitizer.yml ASan/UBSan job lost coverage: {required}"
    )
intentional_leak_filter = f"'^({'|'.join(intentional_leak_tests)})$'"

def workflow_step(job, name):
    match = re.search(
        rf"(?ms)^      - name: {re.escape(name)}\n(?P<body>.*?)(?=^      - name: |\Z)",
        job,
    )
    assert match, f"linux-sanitizer.yml lost step: {name}"
    return match.group("body")

leak_enabled_step = workflow_step(
    asan_job, "Run suite with LeakSanitizer enabled"
)
assert f"-E {intentional_leak_filter}" in leak_enabled_step, (
    "LeakSanitizer-enabled CTest must exclude exactly the five documented tests"
)
assert "detect_leaks=0" not in leak_enabled_step, (
    "LeakSanitizer-enabled CTest must not disable leak detection"
)

intentional_leak_step = workflow_step(
    asan_job, "Run leak-for-safety tests under ASan + UBSan"
)
assert "detect_leaks=0" in intentional_leak_step, (
    "the intentional-leak rerun must disable LeakSanitizer"
)
assert f"-R {intentional_leak_filter}" in intentional_leak_step, (
    "the leak-disabled CTest must rerun exactly the five documented tests"
)

# Source-built libsodium must come from the release asset hosted alongside the
# upstream repository. download.libsodium.org has repeatedly failed DNS lookup
# on GitHub's macOS runners. Keep all static-build workflows on one audited
# version/hash pair, and prevent the unreliable host from being reintroduced.
sodium_pins = {}
expected_sodium_workflows = {
    "macos-build.yml",
    "release.yml",
}
github_sodium_asset = (
    '"https://github.com/jedisct1/libsodium/releases/download/'
    '${SODIUM_VERSION}-RELEASE/libsodium-${SODIUM_VERSION}.tar.gz"'
)
for workflow_path in (source_root / ".github" / "workflows").glob("*.yml"):
    workflow_text = workflow_path.read_text(encoding="utf-8")
    assert "download.libsodium.org" not in workflow_text, (
        f"{workflow_path.name} must not depend on the unreliable libsodium host"
    )
    versions = re.findall(
        r"^\s*SODIUM_VERSION:\s*([^\s#]+)$", workflow_text, re.MULTILINE
    )
    hashes = re.findall(
        r"^\s*SODIUM_SHA256:\s*([0-9a-f]{64})$", workflow_text, re.MULTILINE
    )
    if versions or hashes:
        assert versions and len(versions) == len(hashes), (
            f"{workflow_path.name} must pair every libsodium version with a hash"
        )
        pairs = set(zip(versions, hashes))
        assert len(pairs) == 1, (
            f"{workflow_path.name} has inconsistent libsodium pins: {pairs}"
        )
        assert github_sodium_asset in workflow_text, (
            f"{workflow_path.name} must fetch the official GitHub release asset"
        )
        sodium_pins[workflow_path.name] = next(iter(pairs))
assert set(sodium_pins) == expected_sodium_workflows, (
    f"unexpected source-built libsodium workflow set: {sodium_pins.keys()}"
)
assert len(set(sodium_pins.values())) == 1, (
    f"libsodium version/hash drift across workflows: {sodium_pins}"
)
for guide_name in ("BUILDING-LINUX.md", "BUILDING-WINDOWS.md"):
    guide = (source_root / guide_name).read_text(encoding="utf-8")
    assert (
        f"git -C plugins fetch --depth 1 origin {pinned_donor.group(1)}" in guide
    ), f"{guide_name} must fetch the workflow-pinned donor revision"
    assert "git -C plugins checkout --detach FETCH_HEAD" in guide, (
        f"{guide_name} must check out the fetched donor revision"
    )
linux_guide = (source_root / "BUILDING-LINUX.md").read_text(encoding="utf-8")
assert (
    f'test "$(git -C plugins rev-parse HEAD)" = {pinned_donor.group(1)}'
    in linux_guide
), "BUILDING-LINUX.md must verify the workflow-pinned donor revision"
windows_guide = (source_root / "BUILDING-WINDOWS.md").read_text(encoding="utf-8")
windows_pin_check = (
    'git -C plugins rev-parse HEAD | findstr /x /c:'
    f'"{pinned_donor.group(1)}" >nul || '
    '(echo ERROR: donor checkout did not reach the pinned revision & exit /b 1)'
)
assert windows_pin_check in windows_guide, (
    "BUILDING-WINDOWS.md must fail when the donor revision does not match"
)
cmake_source = (source_root / "CMakeLists.txt").read_text(encoding="utf-8")
assert "DUSKSTUDIO_REQUIRE_ASIO=OFF" not in cmake_source, (
    "CMake must not offer an ASIO-less Windows build"
)
assert "DUSKSTUDIO_REQUIRE_ASIO=OFF" not in windows_guide, (
    "BUILDING-WINDOWS.md must not document an ASIO opt-out"
)
assert "WASAPI is the default" not in windows_guide, (
    "BUILDING-WINDOWS.md must distinguish the required ASIO SDK from runtime driver fallback"
)
assert "This is a build-time\n  requirement." in windows_guide
assert "falls back to WASAPI when no ASIO driver is available" in windows_guide
assert "Every Windows build requires the ASIO SDK." in cmake_source
assert "Windows builds without ASIO are not supported." in cmake_source
assert "target_compile_definitions(DuskStudio PRIVATE JUCE_ASIO=1)" in cmake_source
readme = (source_root / "README.md").read_text(encoding="utf-8")
test_sources = sorted((source_root / "tests").glob("*.cpp"))
test_case_pattern = re.compile(r"^\s*TEST_CASE\s*\(", re.MULTILINE)
test_case_count = sum(
    len(test_case_pattern.findall(path.read_text(encoding="utf-8")))
    for path in test_sources
)
test_source_count = len(test_sources)
suite_summaries = re.findall(
    r"^The C\+\+ suite declares \d+ Catch2 test cases across "
    r"\d+ test source files\.",
    readme,
    re.MULTILINE,
)
expected_suite_summary = (
    f"The C++ suite declares {test_case_count} Catch2 test cases across "
    f"{test_source_count} test source files."
)
assert suite_summaries == [expected_suite_summary], (
    "README must contain exactly one current test-suite summary: "
    f"expected {[expected_suite_summary]!r}, found {suite_summaries!r}"
)
tree_summaries = re.findall(
    r"^tests/\s+# \d+ Catch2 test cases declared in C\+\+ "
    r"\(session, recording, MIDI, IPC, DSP\)$",
    readme,
    re.MULTILINE,
)
expected_tree_summary = (
    f"tests/         # {test_case_count} Catch2 test cases declared in C++ "
    "(session, recording, MIDI, IPC, DSP)"
)
assert tree_summaries == [expected_tree_summary], (
    "README must contain exactly one current source-tree test summary: "
    f"expected {[expected_tree_summary]!r}, found {tree_summaries!r}"
)
release_section_match = re.search(
    r"^## Part 10\b(?P<body>.*)\Z",
    maintainer_guide,
    re.MULTILINE | re.DOTALL,
)
assert release_section_match, (
    "release checklist must point to the documented Part 10 procedure"
)
release_section = release_section_match.group("body")
assert "env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run" in release_section, (
    "Part 10 must retain the local Patreon freshness check"
)
assert ".github/workflows/release.yml" in release_section, (
    "Part 10 must read the donor pin from a release workflow"
)
assert 'cat-file -e "$DONOR_REV^{commit}"' in release_section, (
    "Part 10 must avoid turning the maintainer donor into a shallow checkout"
)
assert 'git -C ../plugins show "$DONOR_REV:plugins/shared/PatreonBackers.h"' in release_section, (
    "Part 10 must print the supporter header from the pinned donor revision"
)
assert "STOP: release workflow has no valid DONOR_REV" in release_section, (
    "Part 10 must stop instead of reading the donor index when pin parsing fails"
)
patreon_command = "env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run"
patreon_at = release_section.index(patreon_command)
patreon_block_start = release_section.rfind("```bash", 0, patreon_at)
patreon_block_end = release_section.find("```", patreon_at)
assert patreon_block_start >= 0 and patreon_block_end >= 0, (
    "Part 10 Patreon freshness command must remain inside a bash code block"
)
patreon_block = release_section[patreon_block_start:patreon_block_end]
assert "set -e" in patreon_block, (
    "Part 10 must abort the Patreon block when donor pin validation fails"
)
override_stop = (
    "STOP: local Patreon name_overrides are not available to release workflows"
)
assert override_stop in patreon_block, (
    "Part 10 must stop when local display-name overrides would diverge from CI"
)
ordered_patreon_steps = [
    "set -e",
    override_stop,
    "DONOR_REV=$(sed -nE",
    '[[ "$DONOR_REV" =~ ^[0-9a-f]{40}$ ]]',
    'cat-file -e "$DONOR_REV^{commit}"',
    'git -C ../plugins show "$DONOR_REV:plugins/shared/PatreonBackers.h"',
    patreon_command,
]
step_positions = [patreon_block.index(step) for step in ordered_patreon_steps]
assert step_positions == sorted(step_positions), (
    "Part 10 must validate local naming and the donor pin before comparing tiers"
)
packaging_guide = (source_root / "packaging" / "README.md").read_text(encoding="utf-8")
assert "Maintainer Guide Part 10" in packaging_guide, (
    "packaging checklist must link the Patreon freshness procedure"
)
assert "MAINTAINER-GUIDE.md#part-10---release" in packaging_guide, (
    "packaging checklist must link directly to Part 10"
)
assert "env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run" in packaging_guide, (
    "packaging checklist must retain the Patreon freshness command"
)
assert "workflow-pinned donor" in packaging_guide, (
    "packaging checklist must compare against the donor revision that ships"
)
assert "missing Patreon" in packaging_guide, (
    "packaging checklist must stop a stale committed-list fallback"
)
handoff_guide = (source_root / "docs" / "handoff-013-milestone.md").read_text(
    encoding="utf-8"
)
assert "env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run" in handoff_guide, (
    "0.13 handoff must retain the safe Patreon freshness command"
)
assert ".codex/worktrees/*" in handoff_guide, (
    "0.13 handoff must keep the primary-checkout requirement"
)

# The sole publisher takes the body from one file in the tagged tree.
notes_path = source_root / "packaging" / "RELEASE-NOTES.md"
assert notes_path.is_file(), "packaging/RELEASE-NOTES.md must exist"
notes_text = notes_path.read_text(encoding="utf-8")
assert notes_text.count(SUMMARY_START) == 1, (
    "tracked notes must contain exactly one summary start marker"
)
assert notes_text.count(SUMMARY_END) == 1, (
    "tracked notes must contain exactly one summary end marker"
)
assert notes_text.index(SUMMARY_START) < notes_text.index(SUMMARY_END), (
    "tracked notes summary markers must be in order"
)
_, tracked_slot, tracked_after = summary_slot(notes_text)
visible_summary = re.sub(r"<!--.*?-->", "", tracked_slot, flags=re.S).strip()
assert visible_summary, "release-summary slot must carry visible content"
assert "### Downloads" in tracked_after, "tracked notes must keep the Downloads section"

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
assert (
    f'CHANGELOG.md "## [9.9.9] - Unreleased" -> '
    f'"## [9.9.9] - {release_date}"'
) in lines["1"]
assert "env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run" in lines["2"], (
    "release checklist must print the Patreon freshness command"
)
assert "MAINTAINER-GUIDE Part 10" in lines["2"]
assert "commit in the plugins repo" not in bump_output
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
assert "--jq '.body // \"\"'" in verifier_text, (
    "verifier must normalize a null release body to empty"
)
expected_assets = re.findall(r'^\s+"[^"\n]+\|[^"\n]+"$', verifier_text, re.MULTILINE)
expected_asset_count = 6
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

for retired in (
    "linux-release.yml",
    "macos-release.yml",
    "windows-build.yml",
    "manual-pdf.yml",
):
    assert not (source_root / ".github" / "workflows" / retired).exists(), (
        f"retired independent release publisher still exists: {retired}"
    )

workflow_name = re.search(r"^name:\s*(.+)$", release_workflow, re.MULTILINE)
assert workflow_name and workflow_name.group(1).strip() == workflows["release.yml"], (
    "release.yml display name drifted from the printed release checklist"
)
assert release_workflow.count(preflight_marker) == 1, (
    "release token preflight must appear exactly once"
)
for action, (revision, expected_count) in action_pins.items():
    refs = re.findall(rf"uses:\s*{re.escape(action)}@([^\s#]+)", release_workflow)
    assert refs == [revision] * expected_count, (
        f"{action} must use its audited commit SHA {expected_count} times: {refs}"
    )
assert not re.search(r"uses:\s*actions/[^@\s]+@v[0-9]+\b", release_workflow), (
    "release credentials must not be exposed to actions pinned by a floating major"
)
preflight_at = release_workflow.index(preflight_marker)
tag_check_at = release_workflow.index("- name: Verify tag matches VERSION")
assert preflight_at < tag_check_at, (
    "token preflight must run before release validation/build work"
)
preflight_block = release_workflow[preflight_at:tag_check_at]
for required in (
    "set -euo pipefail",
    "if: ${{ github.ref_type == 'tag' }}",
    "GH_TOKEN: ${{ secrets.RELEASES_REPO_TOKEN }}",
    "preflight_error=$(mktemp -t duskstudio-preflight.XXXXXX)",
    "trap 'rm -f \"$preflight_error\"' EXIT",
    'probe_tag="duskstudio-token-preflight-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}"',
    "if ! gh api --method POST",
    '"repos/${RELEASES_REPO}/releases/generate-notes"',
    '-f "tag_name=${probe_tag}"',
    '-f "target_commitish=main" >/dev/null 2>"$preflight_error"; then',
    "write-capability preflight for ${RELEASES_REPO} failed",
    'cat "$preflight_error" >&2',
    "exit 1",
):
    assert required in preflight_block, (
        f"release token preflight lost required check: {required}"
    )

jobs_text = release_workflow.split("\njobs:\n", 1)[1]
job_names = re.findall(r"^  ([a-z][a-z0-9-]*):$", jobs_text, re.MULTILINE)
assert set(job_names) == {"preflight", "linux", "macos", "windows", "manual", "publish"}, (
    f"release workflow job set drifted: {job_names}"
)

def release_job(name):
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\n(?P<body>.*?)(?=^  [a-z][a-z0-9-]*:\n|\Z)",
        jobs_text,
    )
    assert match, f"release.yml lost the {name} job"
    return match.group("body")

artifact_contract = {
    "linux": ("release-linux-${{ matrix.arch }}", "dusk-studio-*-Linux-${{ matrix.arch }}.tar.xz"),
    "macos": ("release-macos", '"*.dmg"'),
    "windows": ("release-windows", '"*.msi"'),
    "manual": ("release-manual", "MANUAL.pdf"),
}
upload_artifact_ref = f"actions/upload-artifact@{action_pins['actions/upload-artifact'][0]}"
for job_name, (artifact_name, artifact_path) in artifact_contract.items():
    body = release_job(job_name)
    assert "needs: preflight" in body, f"{job_name} must wait for token/tag preflight"
    assert body.count(upload_artifact_ref) == 1, (
        f"{job_name} must stage exactly one release artifact"
    )
    for required in (
        f"name: {artifact_name}",
        f"path: {artifact_path}",
        "if-no-files-found: error",
        "retention-days: 1",
    ):
        assert required in body, f"{job_name} artifact contract lost: {required}"
    assert "gh release" not in body, f"{job_name} must never publish independently"

for job_name in ("linux", "macos", "windows"):
    body = release_job(job_name)
    app_configure = body.split("- name: Configure", 1)[1].split("- name: Build", 1)[0]
    assert "-DDUSKSTUDIO_ENABLE_NATIVE_UI=ON" in app_configure, (
        f"{job_name} release app must require the native UI"
    )
    assert "for option in DUSKSTUDIO_ENABLE_NATIVE_UI" in app_configure, (
        f"{job_name} release app must verify the native UI cache value"
    )

manual_job = release_job("manual")
for required in (
    "poppler-utils",
    "subject=$(LC_ALL=C pdfinfo MANUAL.pdf",
    '"Dusk Studio ${version} User Manual"',
):
    assert required in manual_job, f"manual version verification lost: {required}"
manual_builder = (source_root / "docs" / "build-pdf.sh").read_text(encoding="utf-8")
assert '--metadata=subject:"Dusk Studio ${VERSION} User Manual"' in manual_builder, (
    "manual builder must embed VERSION in PDF metadata"
)
tsan_suppressions = (source_root / "tools" / "tsan_suppressions.txt").read_text(
    encoding="utf-8"
)
spsc_rationale, separator, _ = tsan_suppressions.partition("race:dusk::SpscIndexFifo")
assert separator and "release stores" in spsc_rationale.rsplit("\n\n", 1)[-1], (
    "the SpscIndexFifo TSan suppression must retain its memory-order rationale"
)

macos_job = release_job("macos")
for required in (
    'for binary in "$APP/Contents/MacOS/DuskStudio" "$HELPER"',
    'otool -L "$binary"',
    'helper_output=$(env -i HOME="$HOME" "$HELPER" 2>&1)',
    "[[ $helper_rc -ne 64 ]]",
    "pass --ipc-stub, --ipc-host or --scan",
):
    assert required in macos_job, f"packaged macOS helper verification lost: {required}"

publish_job = release_job("publish")
for required in (
    "needs: [linux, macos, windows, manual]",
    f"actions/download-artifact@{action_pins['actions/download-artifact'][0]}",
    "pattern: release-*",
    "merge-multiple: true",
    "release fan-in must contain exactly the five expected payloads",
    "SHA256SUMS must contain exactly five entries",
    "sha256sum --check SHA256SUMS",
    "release directory must contain exactly six assets",
    "if: ${{ github.event_name == 'push' && github.ref_type == 'tag' }}",
    notes_marker,
    'gh release upload "$TAG" --repo "$RELEASES_REPO" --clobber dist/*',
    'scripts/verify-release-assets.sh "$TAG"',
):
    assert required in publish_job, f"fan-in publisher lost: {required}"
for payload_name in (
    '"dusk-studio-${version}-Linux-x86_64.tar.xz"',
    '"dusk-studio-${version}-Linux-aarch64.tar.xz"',
    '"dusk-studio-${version}-macOS-arm64.dmg"',
    '"dusk-studio-${version}-Windows-x64.msi"',
    '"MANUAL.pdf"',
):
    assert payload_name in publish_job, f"fan-in lost expected payload: {payload_name}"
assert publish_job.count("gh release create") == 1, (
    "the fan-in publisher must have one release-creation path"
)
assert publish_job.count("gh release upload") == 1, (
    "the fan-in publisher must upload the complete set in one command"
)
upload_at = publish_job.index('gh release upload "$TAG"')
verify_at = publish_job.index('scripts/verify-release-assets.sh "$TAG"')
publish_at = publish_job.index(
    'gh release edit "$TAG" --repo "$RELEASES_REPO" --draft=false'
)
assert publish_job[:upload_at].count("--draft") == 2, (
    "both new and retried releases must be drafts before upload"
)
assert upload_at < verify_at < publish_at, (
    "the release must remain draft until the uploaded six-asset set verifies"
)
assert "SHA256SUMS." not in release_workflow, (
    "per-job checksum fragments must not return"
)
assert "if: ${{ github.ref_type == 'tag' }}" not in publish_job, (
    "a workflow_dispatch targeting a tag must not publish"
)
PY
