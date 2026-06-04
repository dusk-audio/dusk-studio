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

# The body font (Latin Modern, pandoc's xelatex default) lacks the handful of
# symbol glyphs the manual uses - musical note, geometric triangles/discs,
# transport arrows, >=, keyboard, refresh. Map each to a DejaVu Sans fallback
# (ships via fonts-dejavu and covers all of them) so they render instead of
# dropping out. Only these codepoints fall back; the rest stays Latin Modern.
HEADER="${PROCESSED_DIR}/glyph-fallback.tex"
cat > "${HEADER}" <<'TEX'
\usepackage{newunicodechar}
\newfontfamily\glyphfallback{DejaVu Sans}
\newunicodechar{≥}{{\glyphfallback ≥}}
\newunicodechar{⌨}{{\glyphfallback ⌨}}
\newunicodechar{■}{{\glyphfallback ■}}
\newunicodechar{▴}{{\glyphfallback ▴}}
\newunicodechar{▶}{{\glyphfallback ▶}}
\newunicodechar{▾}{{\glyphfallback ▾}}
\newunicodechar{◀}{{\glyphfallback ◀}}
\newunicodechar{◉}{{\glyphfallback ◉}}
\newunicodechar{●}{{\glyphfallback ●}}
\newunicodechar{♩}{{\glyphfallback ♩}}
\newunicodechar{⟳}{{\glyphfallback ⟳}}
\newunicodechar{⚠}{{\glyphfallback ⚠}}

% Cap image height at 0.82 textheight (pandoc's \pandocbounded otherwise
% scales tall figures to the FULL text height, leaving no room for the
% caption - which then collides with the centred footer page number). Width
% bound (\linewidth) is unchanged. Mirrors pandoc 3.x's definition, only the
% height reference shrinks.
%
% \providecommand first so \renewcommand always has a target: pandoc only
% emits \pandocbounded for sized images from ~3.1.7 on, so on a runner with
% an older pandoc the macro is undefined and a bare \renewcommand errors out
% ("Command \pandocbounded undefined"). The provide is a no-op when pandoc
% already defined it; the renew then installs the height-capped version.
\makeatletter
\providecommand*\pandocbounded[1]{#1}%
\renewcommand*\pandocbounded[1]{%
  \sbox\pandoc@box{#1}%
  \Gscale@div\@tempa{0.82\textheight}{\dimexpr\ht\pandoc@box+\dp\pandoc@box\relax}%
  \Gscale@div\@tempb{\linewidth}{\wd\pandoc@box}%
  \ifdim\@tempb\p@<\@tempa\p@\let\@tempa\@tempb\fi%
  \ifdim\@tempa\p@<\p@\scalebox{\@tempa}{\usebox\pandoc@box}%
  \else\usebox\pandoc@box\fi%
}
\makeatother
TEX

pandoc "${PROCESSED}" \
  --output="${OUT}" \
  --pdf-engine=xelatex \
  --resource-path="${REPO_ROOT}:${REPO_ROOT}/docs:${REPO_ROOT}/docs/images" \
  --include-in-header="${HEADER}" \
  --toc \
  --toc-depth=3 \
  --variable=geometry:margin=1in \
  --variable=fontsize=11pt \
  --variable=monofont:"DejaVu Sans Mono" \
  --variable=colorlinks=true

echo "wrote ${OUT}"
