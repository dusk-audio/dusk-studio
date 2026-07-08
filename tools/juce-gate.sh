#!/usr/bin/env bash
# De-JUCE ratchet. Fails if any src/ file gains a JUCE dependency unless it is
# already on tools/juce-allowlist.txt, and fails if a listed file has since
# been cleaned (so the list can only shrink). Migration PRs delete allowlist
# lines; nothing adds them.
#
#   tools/juce-gate.sh            check (CI + pre-push)
#   tools/juce-gate.sh --update   rewrite the allowlist to match reality,
#                                  only after a file was legitimately cleaned
set -euo pipefail
export LC_ALL=C   # sort and comm must agree on collation

cd "$(dirname "$0")/.."
ALLOW=tools/juce-allowlist.txt

current="$(grep -rlE 'juce::|<juce_' src | sort || true)"

if [[ "${1:-}" == "--update" ]]; then
    printf '%s\n' "$current" > "$ALLOW"
    echo "juce-allowlist.txt updated: $(wc -l < "$ALLOW") files still JUCE-coupled."
    exit 0
fi

if [[ ! -f "$ALLOW" ]]; then
    echo "ERROR: $ALLOW missing. Run: tools/juce-gate.sh --update" >&2
    exit 2
fi

allow="$(sort "$ALLOW")"

new="$(comm -23 <(printf '%s\n' "$current") <(printf '%s\n' "$allow"))"
stale="$(comm -13 <(printf '%s\n' "$current") <(printf '%s\n' "$allow"))"

rc=0
if [[ -n "$new" ]]; then
    echo "FAIL: new JUCE coupling in files not on the allowlist:" >&2
    printf '  + %s\n' $new >&2
    echo "Remove the juce:: use, or (last resort) add the file to $ALLOW with a reason." >&2
    rc=1
fi
if [[ -n "$stale" ]]; then
    echo "FAIL: these files are JUCE-free now but still on the allowlist:" >&2
    printf '  - %s\n' $stale >&2
    echo "Delete them from $ALLOW (or run tools/juce-gate.sh --update). The list only shrinks." >&2
    rc=1
fi

count="$(printf '%s\n' "$current" | grep -c . || true)"
echo "JUCE-coupled src files: $count"
exit $rc
