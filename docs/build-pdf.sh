#!/usr/bin/env bash
# Build MANUAL.pdf from MANUAL.md via pandoc + xelatex.
#
# Requirements:
#   - pandoc      (Linux: apt install pandoc; macOS: brew install pandoc)
#   - xelatex     (Linux: apt install texlive-xetex; macOS: brew install --cask mactex-no-gui)
#
# Output: MANUAL.pdf next to MANUAL.md at the repo root.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${REPO_ROOT}/MANUAL.md"
OUT="${REPO_ROOT}/MANUAL.pdf"

if [[ ! -f "${SRC}" ]]; then
  echo "error: ${SRC} not found" >&2
  exit 1
fi

if ! command -v pandoc >/dev/null 2>&1; then
  echo "error: pandoc not on PATH" >&2
  exit 1
fi

if ! command -v xelatex >/dev/null 2>&1; then
  echo "error: xelatex not on PATH (install texlive-xetex / mactex-no-gui)" >&2
  exit 1
fi

if ! command -v perl >/dev/null 2>&1; then
  echo "error: perl not on PATH (needed to strip em dashes)" >&2
  exit 1
fi

# House style: no em dashes in the published manual. Replace every em dash
# (U+2014) with a spaced hyphen before rendering. En dashes (U+2013, used for
# numeric ranges like 20-400 Hz) are left intact. Done on a temp copy so the
# MANUAL.md source is untouched.
# mktemp --suffix is GNU-only (fails on macOS/BSD). Make a temp dir (portable
# on both) and put a fixed .md name inside so pandoc still sees a .md extension.
PROCESSED_DIR="$(mktemp -d)"
PROCESSED="${PROCESSED_DIR}/MANUAL.md"
trap 'rm -rf "${PROCESSED_DIR}"' EXIT
perl -CSD -pe 's/[ \t]*\x{2014}[ \t]*/ - /g' "${SRC}" > "${PROCESSED}"

pandoc "${PROCESSED}" \
  --output="${OUT}" \
  --pdf-engine=xelatex \
  --resource-path="${REPO_ROOT}:${REPO_ROOT}/docs:${REPO_ROOT}/docs/images" \
  --toc \
  --toc-depth=3 \
  --variable=geometry:margin=1in \
  --variable=fontsize=11pt \
  --variable=monofont:"DejaVu Sans Mono" \
  --variable=colorlinks=true

echo "wrote ${OUT}"
