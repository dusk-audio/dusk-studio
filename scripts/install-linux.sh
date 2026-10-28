#!/bin/sh
# Dusk Studio Linux installer. Run from the extracted tarball directory (the one
# holding this script and the DuskStudio/ program folder).
#
#   ./install.sh                user install  (~/.local, no root)
#   sudo ./install.sh --system  system install (/opt + /usr/local + /usr/share)
#   ./install.sh --uninstall    remove a previous install of the same scope
#
# You do NOT need to install at all — DuskStudio/DuskStudio runs in place. The
# installer just adds a launcher to your menu/PATH and registers the session
# file association so double-clicking a session.json opens Dusk Studio.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC="$SCRIPT_DIR/DuskStudio"

SCOPE=user
DO_UNINSTALL=0
for arg in "$@"; do
    case "$arg" in
        --system)    SCOPE=system ;;
        --uninstall) DO_UNINSTALL=1 ;;
        --user)      SCOPE=user ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

if [ "$SCOPE" = system ]; then
    OPT=/opt/dusk-studio
    BINDIR=/usr/local/bin
    DATA=/usr/share
    [ "$(id -u)" = 0 ] || { echo "error: --system needs root (use sudo)" >&2; exit 1; }
else
    # Must run before $HOME is dereferenced: under set -u an unset HOME would
    # abort with "HOME: unbound variable" and a cryptic message otherwise.
    [ -n "${HOME:-}" ] || { echo "error: \$HOME is unset - cannot resolve the user install dir" >&2; exit 1; }
    OPT="$HOME/.local/opt/dusk-studio"
    BINDIR="$HOME/.local/bin"
    DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
fi

# Gate the destructive rm -rf "$OPT" below: OPT must be exactly one of the two
# known install dirs. Guards against any surprise path before we delete it.
# ${HOME:-} keeps the pattern set -u-safe for a system install with HOME unset.
case "$OPT" in
    /opt/dusk-studio|"${HOME:-}"/.local/opt/dusk-studio) : ;;
    *) echo "error: refusing destructive op on unexpected install dir: '$OPT'" >&2; exit 1 ;;
esac

DESKTOP="$DATA/applications/audio.dusk.studio.desktop"
ICON="$DATA/icons/hicolor/256x256/apps/DuskStudio.png"
MIME="$DATA/mime/packages/DuskStudio.mime.xml"
META="$DATA/metainfo/DuskStudio.appdata.xml"

refresh_dbs() {
    command -v update-desktop-database >/dev/null 2>&1 && \
        update-desktop-database "$DATA/applications" >/dev/null 2>&1 || true
    command -v update-mime-database >/dev/null 2>&1 && \
        update-mime-database "$DATA/mime" >/dev/null 2>&1 || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && \
        gtk-update-icon-cache -f -t "$DATA/icons/hicolor" >/dev/null 2>&1 || true
}

if [ "$DO_UNINSTALL" = 1 ]; then
    rm -rf "$OPT"
    rm -f "$BINDIR/DuskStudio" "$DESKTOP" "$ICON" "$MIME" "$META"
    refresh_dbs
    echo "Removed Dusk Studio ($SCOPE)."
    exit 0
fi

[ -x "$SRC/DuskStudio" ] || { echo "error: $SRC/DuskStudio not found - run install.sh from the extracted tarball" >&2; exit 1; }

# The desktop entry launches through env for a path holding '%' and directly
# for one holding '=' (see the Exec rewrite below); neither form starts a path
# holding both.
case "$OPT" in
    *=*%*|*%*=*)
        echo "error: the install path '$OPT' holds both '=' and '%', which no desktop launcher can start; install with --system or from a HOME without one of them" >&2
        exit 1 ;;
esac

# Program dir.
rm -rf "$OPT"
mkdir -p "$OPT"
cp -a "$SRC"/. "$OPT"/
# Settings > Quickstart looks beside the executable; the tarball keeps its copy
# at the top level, beside this script.
cp "$SCRIPT_DIR/QUICKSTART.md" "$OPT/QUICKSTART.md"
BIN="$OPT/DuskStudio"
chmod +x "$BIN" "$OPT/dusk-studio-plugin-host" 2>/dev/null || true

# Launcher on PATH.
mkdir -p "$BINDIR"
ln -sf "$BIN" "$BINDIR/DuskStudio"

# Desktop integration: rewrite the relative Exec to the installed absolute path.
SRC_DESKTOP="$OPT/share/applications/audio.dusk.studio.desktop"
[ -f "$SRC_DESKTOP" ] || { echo "error: $SRC_DESKTOP missing - tarball is incomplete" >&2; exit 1; }
mkdir -p "$(dirname "$DESKTOP")" "$(dirname "$ICON")" "$(dirname "$MIME")" "$(dirname "$META")"
# Desktop values are unescaped before Exec quoting; literal backslashes need
# four escapes, and field-code percent signs need doubling. env keeps desktop
# parsers from checking the executable path before expanding literal percents,
# but GNU env takes any operand holding '=' for an assignment, so such a path
# is launched directly.
BIN="$BIN" awk '
    /^Exec=/ {
        bin = ENVIRON["BIN"]
        escaped = ""
        slash = sprintf("%c", 92)
        for (i = 1; i <= length(bin); ++i) {
            c = substr(bin, i, 1)
            if (c == slash) c = slash slash slash slash
            else if (c == "\"" || c == "`" || c == "$") c = slash slash c
            else if (c == "%") c = "%%"
            else if (c == "\n") c = slash "n"
            else if (c == "\t") c = slash "t"
            else if (c == "\r") c = slash "r"
            escaped = escaped c
        }
        if (index(bin, "=")) print "Exec=\"" escaped "\" %f"
        else print "Exec=env \"" escaped "\" %f"
        next
    }
    { print }
' \
    "$SRC_DESKTOP" > "$DESKTOP"
cp "$OPT/share/icons/hicolor/256x256/apps/DuskStudio.png" "$ICON"
cp "$OPT/share/mime/packages/DuskStudio.mime.xml" "$MIME"
cp "$OPT/share/metainfo/DuskStudio.appdata.xml" "$META"
refresh_dbs

echo "Installed Dusk Studio ($SCOPE) to $OPT"
echo "Launcher: $BINDIR/DuskStudio"
case ":$PATH:" in
    *":$BINDIR:"*) : ;;
    *) echo "Note: $BINDIR is not on your PATH - add it, or launch from your app menu." ;;
esac
