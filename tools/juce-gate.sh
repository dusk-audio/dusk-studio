#!/usr/bin/env bash
# De-JUCE ratchet. Three rules, all one-way:
#
#   1. No src/ file may gain a JUCE dependency unless it is already listed in
#      tools/juce-allowlist.txt.
#   2. No listed file may carry MORE `juce::` / `<juce_` occurrences than the
#      count recorded beside it. Rule 1 alone only guards files that are already
#      clean, so new coupling lands in the listed files instead - which is how
#      most of it actually arrives.
#   3. A listed file that is now JUCE-free must be removed from the list.
#
# Migration work shrinks counts and deletes lines. tools/juce-gate.sh --update
# rewrites the file to match reality; review that diff, every number must go
# down. One sanctioned exception raises a count: a genuinely new UI source file,
# which has no JUCE-free option until the GUI tower lands its toolkit - call it
# out in the PR body.
#
#   tools/juce-gate.sh            check (CI + pre-push)
#   tools/juce-gate.sh --update   rewrite the allowlist to match reality
set -euo pipefail
export LC_ALL=C   # sort and comm must agree on collation

cd "$(dirname "$0")/.."
ALLOW=tools/juce-allowlist.txt

# Occurrences, not matching lines: a single line can introduce several uses, and
# grep -c would score it as one.
juce_count() { grep -oE 'juce::|<juce_' "$1" 2>/dev/null | wc -l | tr -d ' '; }

scan() {
    local f
    grep -rlE 'juce::|<juce_' src | sort | while IFS= read -r f; do
        printf '%s\t%s\n' "$f" "$(juce_count "$f")"
    done
}

current="$(scan)"

if [[ "${1:-}" == "--update" ]]; then
    printf '%s\n' "$current" > "$ALLOW"
    echo "juce-allowlist.txt updated: $(grep -c . "$ALLOW" || true) files, $(awk -F'\t' '{ n += $2 } END { print n+0 }' "$ALLOW") juce:: occurrences."
    exit 0
fi

if [[ ! -f "$ALLOW" ]]; then
    echo "ERROR: $ALLOW missing. Run: tools/juce-gate.sh --update" >&2
    exit 2
fi

allow="$(sort "$ALLOW")"

currentPaths="$(printf '%s\n' "$current" | cut -f1 | grep -v '^$' || true)"
allowPaths="$(printf '%s\n' "$allow"   | cut -f1 | grep -v '^$' || true)"

new="$(comm -23 <(printf '%s\n' "$currentPaths") <(printf '%s\n' "$allowPaths"))"
stale="$(comm -13 <(printf '%s\n' "$currentPaths") <(printf '%s\n' "$allowPaths"))"

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

# Rule 2. Only files present in both lists; rules 1 and 3 already reported the
# rest, and reporting them twice buries the real regression.
grown=""
while IFS=$'\t' read -r path count; do
    [[ -z "$path" ]] && continue
    ceiling="$(printf '%s\n' "$allow" | awk -F'\t' -v p="$path" '$1 == p { print $2; exit }')"
    [[ -z "$ceiling" ]] && continue
    if (( count > ceiling )); then
        grown+="  ~ $path: $ceiling -> $count"$'\n'
    fi
done < <(printf '%s\n' "$current")

if [[ -n "$grown" ]]; then
    echo "FAIL: these files gained JUCE uses (allowlisted files ratchet DOWN only):" >&2
    printf '%s' "$grown" >&2
    echo "Use the dusk:: seams in src/foundation (Fs.h, Text.h, Json.h, MidiBuffer.h," >&2
    echo "SmoothedValue.h, Decibels.h) or a std:: equivalent. Common swaps:" >&2
    echo "  juce::jlimit -> std::clamp                juce::Array<T> -> std::vector<T>" >&2
    echo "  juce::Thread::sleep -> std::this_thread::sleep_for" >&2
    echo "  juce::String member -> std::string        juce::exactlyEqual(a,0) -> a<0||a>0" >&2
    rc=1
fi

files="$(printf '%s\n' "$currentPaths" | grep -c . || true)"
uses="$(printf '%s\n' "$current" | awk -F'\t' 'NF > 1 { n += $2 } END { print n+0 }')"
echo "JUCE-coupled src files: $files ($uses uses)"
exit $rc
