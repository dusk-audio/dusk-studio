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

pandoc "${SRC}" \
  --output="${OUT}" \
  --pdf-engine=xelatex \
  --resource-path="${REPO_ROOT}:${REPO_ROOT}/docs:${REPO_ROOT}/docs/images" \
  --toc \
  --toc-depth=3 \
  --variable=geometry:margin=1in \
  --variable=fontsize=11pt \
  --variable=colorlinks=true

echo "wrote ${OUT}"
