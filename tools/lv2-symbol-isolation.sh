#!/usr/bin/env bash
# Fails when an ELF binary exports any lilv_, serd_, sord_, sratom_ or zix_
# symbol. A binary that defines them overrides the system LV2 libraries it also
# loads, so their calls land in its own copy instead.
#
#   tools/lv2-symbol-isolation.sh BINARY...
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 BINARY..." >&2
    exit 2
fi

status=0
for binary in "$@"; do
    if [[ ! -f "$binary" ]]; then
        echo "error: no such binary: $binary" >&2
        status=1
        continue
    fi
    symbols="$(nm -D --defined-only "$binary")" || { echo "error: nm failed on $binary" >&2; status=1; continue; }
    exported="$(awk '{ print $NF }' <<<"$symbols" | grep -E '^(lilv|serd|sord|sratom|zix)_' || true)"
    if [[ -n "$exported" ]]; then
        echo "error: $binary exports $(wc -l <<<"$exported") LV2 library symbols, for example:" >&2
        head -5 <<<"$exported" | sed 's/^/  /' >&2
        status=1
    else
        echo "ok: $binary exports no LV2 library symbols"
    fi
done
exit "$status"
