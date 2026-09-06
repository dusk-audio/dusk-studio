#!/usr/bin/env bash
# Requires VERSION, the top CHANGELOG heading, the top AppStream release entry
# and the release-notes summary to agree, in whichever of the two states the
# tree is allowed to be in: development (top heading Unreleased, VERSION at the
# last dated heading) or release-ready (top heading dated, everything at that
# version and date, the date not in the future).
#
#   scripts/release-metadata-check.sh                       consistency only
#   scripts/release-metadata-check.sh --date 2026-09-08     the heading must carry this date
#   scripts/release-metadata-check.sh --tag v0.13.3 --commit-date 2026-09-08
#       the tag must equal VERSION and the heading date must be within a day
#       of the tagged commit
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG=""
DATE=""
COMMIT_DATE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag) TAG="$2"; shift 2 ;;
        --date) DATE="$2"; shift 2 ;;
        --commit-date) COMMIT_DATE="$2"; shift 2 ;;
        --root) ROOT="$2"; shift 2 ;;
        *)
            echo "usage: $0 [--root <dir>] [--tag vX.Y.Z] [--date YYYY-MM-DD] [--commit-date YYYY-MM-DD]" >&2
            exit 2
            ;;
    esac
done

fail() {
    echo "release metadata: $*" >&2
    exit 1
}

# GNU and BSD date disagree on how to parse a date, and this runs on both, so
# the calendar work goes through Python.
epochOf() {
    python3 -c 'import datetime, sys
d = datetime.datetime.strptime(sys.argv[1], "%Y-%m-%d").replace(tzinfo=datetime.timezone.utc)
print(int(d.timestamp()))' "$1"
}

isDate() {
    [[ "$1" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] && epochOf "$1" >/dev/null 2>&1
}

version="$(tr -d '[:space:]' < "$ROOT/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "VERSION reads '$version', not X.Y.Z"

top="$(grep -m1 -E '^## \[[0-9]+\.[0-9]+\.[0-9]+\] - ' "$ROOT/CHANGELOG.md" || true)"
[[ -n "$top" ]] || fail "CHANGELOG.md has no '## [X.Y.Z] - ...' heading"
topVersion="$(sed -E 's/^## \[([^]]+)\] - .*/\1/' <<<"$top")"
topDate="$(sed -E 's/^## \[[^]]+\] - (.*)$/\1/' <<<"$top" | tr -d '[:space:]')"

appLine="$(grep -m1 -E '<release version="[^"]+" date="[^"]+"' "$ROOT/packaging/DuskStudio.appdata.xml" || true)"
[[ -n "$appLine" ]] || fail "packaging/DuskStudio.appdata.xml has no <release version= date=> entry"
appVersion="$(sed -E 's/.*version="([^"]+)".*/\1/' <<<"$appLine")"
appDate="$(sed -E 's/.*date="([^"]+)".*/\1/' <<<"$appLine")"
isDate "$appDate" || fail "AppStream release date '$appDate' is not YYYY-MM-DD"

summary="$(sed -n '/<!-- summary-start -->/,/<!-- summary-end -->/p' "$ROOT/packaging/RELEASE-NOTES.md" \
    | grep -v '<!--' | grep -v '^[[:space:]]*$' || true)"
[[ -n "$summary" ]] || fail "packaging/RELEASE-NOTES.md has an empty summary slot"

if [[ "$topDate" == "Unreleased" ]]; then
    [[ -z "${TAG}${DATE}${COMMIT_DATE}" ]] \
        || fail "CHANGELOG heads [$topVersion] as Unreleased; a release needs it dated"
    [[ "$topVersion" != "$version" ]] \
        || fail "CHANGELOG heads [$topVersion] as Unreleased but VERSION already reads $version"
    [[ "$appVersion" == "$version" ]] \
        || fail "AppStream's top release is $appVersion, VERSION reads $version"
    last="$(grep -m1 -E '^## \[[0-9]+\.[0-9]+\.[0-9]+\] - [0-9]{4}-[0-9]{2}-[0-9]{2}' "$ROOT/CHANGELOG.md" \
        | sed -E 's/^## \[([^]]+)\].*/\1/' || true)"
    [[ -z "$last" || "$last" == "$version" ]] \
        || fail "the last dated CHANGELOG heading is [$last], VERSION reads $version"
    echo "release metadata: development state, VERSION $version, [$topVersion] unreleased"
    exit 0
fi

isDate "$topDate" || fail "CHANGELOG heading [$topVersion] carries '$topDate', neither a date nor Unreleased"
[[ "$topVersion" == "$version" ]] || fail "CHANGELOG heads [$topVersion], VERSION reads $version"
[[ "$appVersion" == "$version" ]] || fail "AppStream's top release is $appVersion, VERSION reads $version"
[[ "$appDate" == "$topDate" ]] || fail "AppStream dates $version $appDate, CHANGELOG dates it $topDate"
today="$(date -u +%F)"
[[ ! "$topDate" > "$today" ]] || fail "CHANGELOG dates $version $topDate, after today ($today)"

if [[ -n "$TAG" ]]; then
    [[ "$TAG" == "v$version" ]] || fail "tag $TAG does not match VERSION v$version"
fi
if [[ -n "$DATE" ]]; then
    isDate "$DATE" || fail "--date '$DATE' is not YYYY-MM-DD"
    [[ "$DATE" == "$topDate" ]] || fail "CHANGELOG dates $version $topDate; the release day is $DATE"
fi
if [[ -n "$COMMIT_DATE" ]]; then
    isDate "$COMMIT_DATE" || fail "--commit-date '$COMMIT_DATE' is not YYYY-MM-DD"
    delta=$(( ( $(epochOf "$COMMIT_DATE") - $(epochOf "$topDate") ) / 86400 ))
    (( delta >= -1 && delta <= 1 )) \
        || fail "CHANGELOG dates $version $topDate; the tagged commit is from $COMMIT_DATE"
fi
echo "release metadata: release-ready, $version dated $topDate"
