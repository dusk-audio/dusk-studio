#!/usr/bin/env bash
# Exercises scripts/verify-package-contents.sh against synthetic trees. A
# checker that cannot fail is not a check, so every case here proves one way it
# reports a problem, and the happy path proves it still passes when it should.

set -euo pipefail

REPO_ROOT="${1:?usage: package_contents.sh <repo-root> <cmake>}"
CMAKE="${2:?usage: package_contents.sh <repo-root> <cmake>}"
SCRIPT="${REPO_ROOT}/scripts/verify-package-contents.sh"
[[ -x "$SCRIPT" ]] || { echo "not executable: $SCRIPT" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

CONTRACT="$WORK/contract.txt"
cat > "$CONTRACT" <<'CONTRACT_EOF'
# comment line
linux	App/Binary
linux	LICENSE

macos	Bundle.app
windows	bin/Binary.exe
CONTRACT_EOF
export DUSKSTUDIO_CONTENTS_CONTRACT="$CONTRACT"

mkdir -p "$WORK/good/App"
touch "$WORK/good/App/Binary" "$WORK/good/LICENSE"
"$SCRIPT" linux "$WORK/good" >/dev/null || fail "a complete package was rejected"

mkdir -p "$WORK/short/App"
touch "$WORK/short/App/Binary"
if "$SCRIPT" linux "$WORK/short" >/dev/null 2>"$WORK/err"; then
    fail "a package missing LICENSE was accepted"
fi
grep -q "LICENSE" "$WORK/err" || fail "the missing path was not named"

# Every gap in one run, not just the first.
mkdir -p "$WORK/empty"
"$SCRIPT" linux "$WORK/empty" >/dev/null 2>"$WORK/err2" && fail "an empty package was accepted"
grep -q "missing 2 required" "$WORK/err2" || fail "did not report both gaps at once"

# A macOS drag install needs the image's Applications link and a quickstart
# inside the bundle, where it survives copying only the .app to /Applications.
MAC_CONTRACT="$WORK/mac-contract.txt"
cat > "$MAC_CONTRACT" <<'MAC_EOF'
macos	Bundle.app
macos	Applications
macos	Bundle.app/Contents/Resources/QUICKSTART.md
MAC_EOF
mkdir -p "$WORK/mac/Bundle.app/Contents/Resources" "$WORK/other"
touch "$WORK/mac/Bundle.app/Contents/Resources/QUICKSTART.md"
ln -s "$WORK/other" "$WORK/mac/Applications"
if DUSKSTUDIO_CONTENTS_CONTRACT="$MAC_CONTRACT" "$SCRIPT" macos "$WORK/mac" \
       >/dev/null 2>"$WORK/mac-err"; then
    fail "an Applications link pointing elsewhere was accepted"
fi
grep -q 'Applications' "$WORK/mac-err" || fail "the wrong Applications link was not named"

rm "$WORK/mac/Applications"
ln -s /Applications "$WORK/mac/Applications"
rm "$WORK/mac/Bundle.app/Contents/Resources/QUICKSTART.md"
mkdir "$WORK/mac/Bundle.app/Contents/Resources/QUICKSTART.md"
if DUSKSTUDIO_CONTENTS_CONTRACT="$MAC_CONTRACT" "$SCRIPT" macos "$WORK/mac" \
       >/dev/null 2>"$WORK/mac-err"; then
    fail "a directory stood in for the installed quickstart"
fi
grep -q 'QUICKSTART.md' "$WORK/mac-err" || fail "the missing quickstart was not named"

rmdir "$WORK/mac/Bundle.app/Contents/Resources/QUICKSTART.md"
touch "$WORK/mac/Bundle.app/Contents/Resources/QUICKSTART.md"
DUSKSTUDIO_CONTENTS_CONTRACT="$MAC_CONTRACT" "$SCRIPT" macos "$WORK/mac" >/dev/null \
    || fail "a complete macOS package was rejected"

# Windows matches by name anywhere, because 7z flattens an MSI.
mkdir -p "$WORK/win/some/nested/dir"
touch "$WORK/win/some/nested/dir/Binary.exe"
"$SCRIPT" windows "$WORK/win" >/dev/null || fail "a flattened Windows extraction was rejected"

# The shapes a real `7z x` of our MSI actually produces: a CM_FP_ prefix, the
# install directory folded into the name with a dot, hyphens rewritten as
# underscores, and root files carrying no directory token at all.
REALISTIC="$WORK/realistic.txt"
cat > "$REALISTIC" <<'REAL_EOF'
windows	bin/DuskStudio.exe
windows	bin/dusk-studio-plugin-host.exe
windows	LICENSE
windows	LICENSES.txt
REAL_EOF
mkdir -p "$WORK/msi"
touch "$WORK/msi/CM_FP_bin.DuskStudio.exe" \
      "$WORK/msi/CM_FP_bin.dusk_studio_plugin_host.exe" \
      "$WORK/msi/CM_FP_LICENSE" \
      "$WORK/msi/CM_FP_LICENSES.txt"
DUSKSTUDIO_CONTENTS_CONTRACT="$REALISTIC" "$SCRIPT" windows "$WORK/msi" >/dev/null \
    || fail "a realistic MSI extraction was rejected"

# LICENSE must not be satisfied by LICENSES.txt: the boundary rule is what
# stops one required file standing in for another.
rm "$WORK/msi/CM_FP_LICENSE"
if DUSKSTUDIO_CONTENTS_CONTRACT="$REALISTIC" "$SCRIPT" windows "$WORK/msi" \
       >/dev/null 2>"$WORK/err3"; then
    fail "LICENSES.txt was accepted in place of LICENSE"
fi
grep -q "LICENSE$" "$WORK/err3" || fail "the missing LICENSE was not named"

# A Markdown file's relative links must resolve inside the package; URLs and
# anchors are not checked. Both dead links are reported in one run.
DOC_CONTRACT="$WORK/doc-contract.txt"
printf 'linux\tQUICKSTART.md\n' > "$DOC_CONTRACT"
mkdir -p "$WORK/doc"
cat > "$WORK/doc/QUICKSTART.md" <<'DOC_EOF'
See [the manual](MANUAL.md) and [a section](#where), or [ask](https://example.com/q) or [here](//example.com/q).
![Shot](docs/images/shot.png)
DOC_EOF
if DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/doc" \
       >/dev/null 2>"$WORK/doc-err"; then
    fail "a quickstart linking files the package lacks was accepted"
fi
grep -q "QUICKSTART.md -> MANUAL.md" "$WORK/doc-err" || fail "the dead manual link was not named"
grep -q "QUICKSTART.md -> docs/images/shot.png" "$WORK/doc-err" || fail "the dead image link was not named"
mkdir -p "$WORK/doc/docs/images"
touch "$WORK/doc/MANUAL.md" "$WORK/doc/docs/images/shot.png"
DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/doc" >/dev/null \
    || fail "a quickstart whose links all resolve was rejected"

# A target may climb within the package, but not out of it: the packaging host
# having a file there says nothing about the user's machine.
touch "$WORK/outside.md"
printf 'Also [inside](docs/../MANUAL.md).\n' >> "$WORK/doc/QUICKSTART.md"
DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/doc" >/dev/null \
    || fail "a link that climbs but stays inside the package was rejected"
printf 'And [outside](../outside.md).\n' >> "$WORK/doc/QUICKSTART.md"
if DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/doc" \
       >/dev/null 2>"$WORK/doc-err"; then
    fail "a link out of the package root was accepted"
fi
grep -q "QUICKSTART.md -> ../outside.md" "$WORK/doc-err" || fail "the escaping link was not named"

# Reference definitions are links too.
mkdir -p "$WORK/refs"
cat > "$WORK/refs/QUICKSTART.md" <<'REF_EOF'
Read [the guide][guide], then [ask][forum].

[guide]: GUIDE.md
  [forum]: <https://example.com/forum> "Forum"
REF_EOF
if DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/refs" \
       >/dev/null 2>"$WORK/refs-err"; then
    fail "a reference definition to a missing file was accepted"
fi
grep -q "QUICKSTART.md -> GUIDE.md" "$WORK/refs-err" || fail "the dead reference definition was not named"
grep -q "example.com" "$WORK/refs-err" && fail "a URL reference definition was reported"
touch "$WORK/refs/GUIDE.md"
DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/refs" >/dev/null \
    || fail "a reference definition to a packaged file was rejected"

# A symlink inside the package is judged by where it lands, not by its name.
mkdir -p "$WORK/sym"
touch "$WORK/sym/real.md"
ln -s real.md "$WORK/sym/inside.md"
ln -s ../outside.md "$WORK/sym/escape.md"
ln -s missing.md "$WORK/sym/broken.md"
printf '[a](inside.md) [b](escape.md) [c](broken.md)\n' > "$WORK/sym/QUICKSTART.md"
if DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/sym" \
       >/dev/null 2>"$WORK/sym-err"; then
    fail "symlinks out of the package or to nothing were accepted"
fi
grep -q "QUICKSTART.md -> escape.md" "$WORK/sym-err" || fail "the escaping symlink was not named"
grep -q "QUICKSTART.md -> broken.md" "$WORK/sym-err" || fail "the broken symlink was not named"
grep -q "QUICKSTART.md -> inside.md" "$WORK/sym-err" && fail "a symlink inside the package was rejected"

printf 'windows\tQUICKSTART.md\n' > "$WORK/win-doc-contract.txt"
mkdir -p "$WORK/win-doc"
printf 'Read [the licence](LICENSE).\n' > "$WORK/win-doc/CM_FP_QUICKSTART.md"
if DUSKSTUDIO_CONTENTS_CONTRACT="$WORK/win-doc-contract.txt" "$SCRIPT" windows "$WORK/win-doc" \
       >/dev/null 2>"$WORK/win-doc-err"; then
    fail "a flattened MSI quickstart linking a missing file was accepted"
fi
grep -q "QUICKSTART.md -> LICENSE" "$WORK/win-doc-err" || fail "the dead MSI link was not named"
touch "$WORK/win-doc/CM_FP_LICENSE"
DUSKSTUDIO_CONTENTS_CONTRACT="$WORK/win-doc-contract.txt" "$SCRIPT" windows "$WORK/win-doc" >/dev/null \
    || fail "a flattened MSI quickstart whose link resolves was rejected"

# The real quickstart links paths no package carries, and the copy packagers
# ship must link the tagged source for each of them instead.
mkdir -p "$WORK/repo-doc" "$WORK/shipped-doc"
cp "$REPO_ROOT/QUICKSTART.md" "$WORK/repo-doc/QUICKSTART.md"
DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/repo-doc" >/dev/null 2>&1 \
    && fail "the repository quickstart has no relative links left to rewrite"
"$CMAKE" -DQUICKSTART_IN="$REPO_ROOT/QUICKSTART.md" \
         -DQUICKSTART_OUT="$WORK/shipped-doc/QUICKSTART.md" \
         -DQUICKSTART_VERSION=9.8.7 -P "$REPO_ROOT/scripts/ship-quickstart.cmake" \
    || fail "ship-quickstart.cmake failed"
DUSKSTUDIO_CONTENTS_CONTRACT="$DOC_CONTRACT" "$SCRIPT" linux "$WORK/shipped-doc" >/dev/null \
    || fail "the shipped quickstart still links a path the package lacks"
rewritten=0
while IFS= read -r url; do
    case "$url" in
        https://raw.githubusercontent.com/dusk-audio/dusk-studio/v9.8.7/*)
            relative="${url#https://raw.githubusercontent.com/dusk-audio/dusk-studio/v9.8.7/}" ;;
        https://github.com/dusk-audio/dusk-studio/blob/v9.8.7/*)
            relative="${url#https://github.com/dusk-audio/dusk-studio/blob/v9.8.7/}" ;;
        *) continue ;;
    esac
    [[ -f "$REPO_ROOT/${relative%%#*}" ]] || fail "the shipped quickstart links $url, not in the source tree"
    rewritten=$((rewritten + 1))
done < <(grep -oE '\]\([^)]+\)' "$WORK/shipped-doc/QUICKSTART.md" | sed 's/^](//; s/)$//')
[[ $rewritten -gt 0 ]] || fail "the shipped quickstart links nothing at the tagged source"
grep -q 'raw.githubusercontent.com/dusk-audio/dusk-studio/v9.8.7/docs/images/' "$WORK/shipped-doc/QUICKSTART.md" \
    || fail "shipped quickstart images do not point at raw files"

# Rooted paths are not repository paths and pass through the rewrite untouched.
printf '[a](/documentation) [b](//example.com/x.png) [c](docs/y.png)\n' > "$WORK/rooted.md"
"$CMAKE" -DQUICKSTART_IN="$WORK/rooted.md" -DQUICKSTART_OUT="$WORK/rooted-out.md" \
         -DQUICKSTART_VERSION=9.8.7 -P "$REPO_ROOT/scripts/ship-quickstart.cmake" \
    || fail "ship-quickstart.cmake failed on rooted paths"
grep -qF '[a](/documentation) [b](//example.com/x.png) [c](https://raw.githubusercontent.com/dusk-audio/dusk-studio/v9.8.7/docs/y.png)' \
    "$WORK/rooted-out.md" || fail "rooted paths were rewritten: $(cat "$WORK/rooted-out.md")"

# A contract this cannot read must stop the release, not skip the record.
printf 'linux only-one-field\n' > "$WORK/bad.txt"
DUSKSTUDIO_CONTENTS_CONTRACT="$WORK/bad.txt" "$SCRIPT" linux "$WORK/good" >/dev/null 2>&1 \
    && fail "a malformed contract was accepted"

# An empty path used to pass vacuously: "$ROOT/" always exists.
printf 'linux\t\n' > "$WORK/emptypath.txt"
if DUSKSTUDIO_CONTENTS_CONTRACT="$WORK/emptypath.txt" "$SCRIPT" linux "$WORK/good" \
       >/dev/null 2>"$WORK/err4"; then
    fail "a record with an empty path was accepted"
fi
grep -q "emptypath.txt:1: record has an empty path" "$WORK/err4" \
    || fail "the empty-path record was not reported with its contract line"

printf 'solaris\tApp\n' > "$WORK/unknown.txt"
DUSKSTUDIO_CONTENTS_CONTRACT="$WORK/unknown.txt" "$SCRIPT" linux "$WORK/good" >/dev/null 2>&1 \
    && fail "an unknown platform tag was accepted"

# The real contract must parse and cover all three platforms.
unset DUSKSTUDIO_CONTENTS_CONTRACT
for platform in linux macos windows; do
    out="$("$SCRIPT" "$platform" "$WORK/empty" 2>&1 || true)"
    grep -q "required path" <<< "$out" \
        || fail "the shipped contract lists nothing for $platform: $out"
done

echo "package-contents checker OK"
