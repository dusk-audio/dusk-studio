#!/usr/bin/env bash
# Exercises scripts/verify-package-contents.sh against synthetic trees. A
# checker that cannot fail is not a check, so every case here proves one way it
# reports a problem, and the happy path proves it still passes when it should.

set -euo pipefail

REPO_ROOT="${1:?usage: package_contents.sh <repo-root>}"
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
