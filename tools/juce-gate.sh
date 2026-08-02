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
# re-records reality, but it is a ratchet too: it refuses to add a path or raise
# a count, so it cannot be used to launder a regression into the baseline. A
# genuinely unavoidable addition is a hand edit to the allowlist, which shows up
# in review as its own line.
#
#   tools/juce-gate.sh            check (CI + pre-push)
#   tools/juce-gate.sh --update   re-record the allowlist (downwards only)
set -euo pipefail
export LC_ALL=C   # sort and comm must agree on collation

cd "$(dirname "$0")/.."
ALLOW=tools/juce-allowlist.txt

# Occurrences, not matching lines: a single line can introduce several uses, and
# grep -c would score it as one.
juce_count() { grep -oE 'juce::|<juce_' "$1" 2>/dev/null | wc -l | tr -d ' '; }

scan() {
    local files rc f
    files="$(grep -rlE 'juce::|<juce_' src)" && rc=0 || rc=$?
    # grep exits 1 for "no match", which is the end state this whole campaign is
    # aiming at, not a failure - under set -e that would abort the run and take
    # --update with it. Anything above 1 (unreadable path, bad regex) is real.
    if (( rc > 1 )); then
        echo "ERROR: scanning src/ failed (grep exit $rc)" >&2
        return "$rc"
    fi
    [[ -z "$files" ]] && return 0

    printf '%s\n' "$files" | sort | while IFS= read -r f; do
        printf '%s\t%s\n' "$f" "$(juce_count "$f")"
    done
}

current="$(scan)"

write_allow() {
    printf '%s\n' "$current" > "$ALLOW"
    echo "juce-allowlist.txt updated: $(grep -c . "$ALLOW" || true) files, $(awk -F'\t' '{ n += $2 } END { print n+0 }' "$ALLOW") juce:: occurrences."
}

# Bootstrap: with no allowlist there is nothing to ratchet against.
if [[ ! -f "$ALLOW" ]]; then
    if [[ "${1:-}" == "--update" ]]; then
        write_allow
        exit 0
    fi
    echo "ERROR: $ALLOW missing. Run: tools/juce-gate.sh --update" >&2
    exit 2
fi

# Fail closed on a malformed record. A line without its count silently loses
# ratchet coverage for that file (the count lookup below yields nothing and the
# check is skipped), which would let anyone disable rule 2 for a file by
# deleting one field - and is also what a rebase onto the pre-count allowlist
# format looks like. A duplicate path hides the same way: the lookup takes the
# first record, so a second, higher entry is never compared. Counts are decimal
# with no leading zero, because bash arithmetic reads 010 as octal 8.
malformed="$(awk -F'\t' '
    NF == 0 { next }
    (NF != 2 || $1 == "" || $2 !~ /^(0|[1-9][0-9]*)$/) {
        printf "  line %d: malformed record: %s\n", NR, $0; next
    }
    seen[$1]++ { printf "  line %d: duplicate path: %s\n", NR, $1 }
' "$ALLOW")"
if [[ -n "$malformed" ]]; then
    echo "ERROR: $ALLOW has bad records (want one path<TAB>count per file):" >&2
    printf '%s\n' "$malformed" >&2
    echo "Fix them by hand; --update refuses to run against an unreadable list." >&2
    exit 2
fi

allow="$(sort "$ALLOW")"

currentPaths="$(printf '%s\n' "$current" | cut -f1 | grep -v '^$' || true)"
allowPaths="$(printf '%s\n' "$allow"   | cut -f1 | grep -v '^$' || true)"

new="$(comm -23 <(printf '%s\n' "$currentPaths") <(printf '%s\n' "$allowPaths"))"
stale="$(comm -13 <(printf '%s\n' "$currentPaths") <(printf '%s\n' "$allowPaths"))"

# Rule 2. Only files present in both lists; rules 1 and 3 already report the
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

# --update re-records reality, but only downwards. Letting it absorb a new path
# or a raised count would turn the one command everybody runs after a migration
# into the way regressions enter the baseline.
if [[ "${1:-}" == "--update" ]]; then
    if [[ -n "$new" || -n "$grown" ]]; then
        echo "ERROR: --update only ratchets DOWN, and this tree adds coupling:" >&2
        [[ -n "$new" ]]   && printf '  + %s\n' $new >&2
        [[ -n "$grown" ]] && printf '%s' "$grown" >&2
        echo "Remove the new uses first. An addition that is genuinely unavoidable is a" >&2
        echo "hand edit to $ALLOW, so it lands in review as its own line." >&2
        exit 2
    fi
    write_allow
    exit 0
fi

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
