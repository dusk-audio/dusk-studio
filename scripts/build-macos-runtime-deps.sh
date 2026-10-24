#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <private-install-prefix> [jobs, 1-5]" >&2
    exit 2
fi
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || {
    echo "error: an arm64 macOS build host is required" >&2; exit 1;
}
jobs="${2:-5}"
[[ "$jobs" =~ ^[1-5]$ ]] || { echo "error: jobs must be between 1 and 5" >&2; exit 2; }
[[ ! -L "$1" ]] || { echo "error: use a private install prefix" >&2; exit 1; }
prefix="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1")"
[[ "$prefix" != / && "$prefix" != "$HOME" ]] || {
    echo "error: use a private install prefix" >&2; exit 1;
}
work="${prefix}.build"
for directory in "$prefix" "$work"; do
    if [[ -e "$directory" ]]; then
        [[ -f "$directory/.dusk-macos-runtime-deps" && ! -L "$directory" ]] || {
            echo "error: refusing an existing unowned directory: $directory" >&2; exit 1;
        }
    else
        mkdir -p "$directory"
        printf '1\n' >"$directory/.dusk-macos-runtime-deps"
    fi
done
for tool in curl tar shasum cmake ninja meson pkg-config python3 xcrun otool lipo make; do
    command -v "$tool" >/dev/null || { echo "error: missing build tool: $tool" >&2; exit 1; }
done
mkdir -p "$work/archives" "$work/sources" "$work/builds" "$work/logs"
sdk="$(xcrun --sdk macosx --show-sdk-path)"
CC="$(xcrun --find clang)"
CXX="$(xcrun --find clang++)"
export CC CXX
export OBJC="$CC" OBJCXX="$CXX" SDKROOT="$sdk" MACOSX_DEPLOYMENT_TARGET=11.0
export CFLAGS="-O2 -arch arm64 -mmacosx-version-min=11.0 -isysroot $sdk"
export CXXFLAGS="$CFLAGS" OBJCFLAGS="$CFLAGS" OBJCXXFLAGS="$CFLAGS"
export CPPFLAGS="-I$prefix/include"
export LDFLAGS="-L$prefix/lib -arch arm64 -mmacosx-version-min=11.0 -isysroot $sdk"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/share/pkgconfig" PKG_CONFIG_PATH=
export CMAKE_PREFIX_PATH="$prefix"
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH CMAKE_LIBRARY_PATH CMAKE_INCLUDE_PATH
unset DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH
export XDG_CACHE_HOME="$work/cache" TMPDIR="$work/tmp"
mkdir -p "$XDG_CACHE_HOME" "$TMPDIR"

# These are the source versions in the macOS release dependency inventory.
cat >"$work/sources.tsv" <<'SOURCES'
ogg	1.3.6	https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz	83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638	BSD-3-Clause
vorbis	1.3.7	https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz	b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b	BSD-3-Clause
flac	1.5.0	https://downloads.xiph.org/releases/flac/flac-1.5.0.tar.xz	f2c1c76592a82ffff8413ba3c4a1299b6c7ab06c734dee03fd88630485c2b920	BSD-3-Clause
opus	1.6.1	https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz	6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1	BSD-3-Clause
mpg123	1.33.7	https://www.mpg123.de/download/mpg123-1.33.7.tar.bz2	31d0e35a4ca567ec9b5ebda6c3062bb4435d6d3eacd6ef0d95cadd7854dc03ee	LGPL-2.1-only
lame	4.0	https://downloads.sourceforge.net/project/lame/lame/4.0/lame-4.0.tar.gz	3df5124d5ad3a98312ffd7ba6a9b36230e4f8a3e66d3ce0f425e336c32d216eb	LGPL-2.0-or-later
sndfile	1.2.2	https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz	3799ca9924d3125038880367bf1468e53a1b7e3686a934f098b7e1d286cdb80e	LGPL-2.1-or-later
lv2	1.18.10	https://lv2plug.in/spec/lv2-1.18.10.tar.xz	78c51bcf21b54e58bb6329accbb4dae03b2ed79b520f9a01e734bd9de530953f	ISC OR 0BSD
zix	0.8.2	https://download.drobilla.net/zix-0.8.2.tar.xz	4c73aab0f8cbbfe56b00c8c6d648316021e699c5ae6cbf254391ef309047e67b	ISC
serd	0.32.10	https://download.drobilla.net/serd-0.32.10.tar.xz	b0e93b49e52f01a049475b7886ef140407115a32d3b1e5dc5f95141c88275d1c	ISC
sord	0.16.22	https://download.drobilla.net/sord-0.16.22.tar.xz	bb23b34b216579136795d518cffa73d91cf205594ce9accebfd408afb839173f	ISC
sratom	0.6.22	https://download.drobilla.net/sratom-0.6.22.tar.xz	0209b7d0f22c96abb416722ed735b0933be47931ecff4aa4b26ded7760b4f252	ISC
lilv	0.28.0	https://download.drobilla.net/lilv-0.28.0.tar.xz	8dcb70adb5cf072335115a6b091f4113710bdc73abaadaa3f9e9c1e55957b149	ISC
suil	0.10.26	https://download.drobilla.net/suil-0.10.26.tar.xz	62808916602c47d201a1ec2d246323a8048243f2bf972f859f0db1db4662ee43	ISC
SOURCES

prepare_source() {
    local name="$1" url="$2" digest="$3" archive
    archive="$work/archives/${url##*/}"
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --proto '=https' --proto-redir '=https' \
            --retry 3 --retry-all-errors --connect-timeout 30 --max-time 300 \
            --output "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 --check
    source_dir="$work/sources/$name"
    [[ ! -L "$source_dir" ]] || { echo "error: source directory is a symlink: $source_dir" >&2; return 1; }
    rm -rf "$source_dir"
    mkdir "$source_dir"
    tar -xf "$archive" -C "$source_dir" --strip-components=1
}

build_cmake() {
    cmake -S "$source_dir" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_INSTALL_NAME_DIR="$prefix/lib" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_IGNORE_PREFIX_PATH='/opt/homebrew;/usr/local' \
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
        -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON \
        -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" -DPKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" \
        -DBUILD_SHARED_LIBS=ON "$@" || return
    cmake --build "$build_dir" -j"$jobs" || return
    cmake --install "$build_dir"
}

build_meson() {
    meson setup "$build_dir" "$source_dir" \
        --prefix="$prefix" --libdir=lib --buildtype=release --default-library=shared \
        --wrap-mode=nofallback -Ddocs=disabled -Dtests=disabled "$@" || return
    meson compile -C "$build_dir" -j "$jobs" || return
    meson install -C "$build_dir"
}

build_autotools() {
    mkdir -p "$build_dir" || return
    (
        cd "$build_dir" || exit
        "$source_dir/configure" --prefix="$prefix" --libdir="$prefix/lib" \
            --enable-shared --disable-static --disable-dependency-tracking "$@" || exit
        make -j"$jobs" || exit
        make install
    )
}

while IFS=$'\t' read -r name version url digest license; do
    echo "Building $name $version ($license) for arm64 macOS 11.0"
    prepare_source "$name" "$url" "$digest"
    build_dir="$work/builds/$name"
    [[ ! -L "$build_dir" ]] || { echo "error: build directory is a symlink: $build_dir" >&2; exit 1; }
    rm -rf "$build_dir"
    {
        case "$name" in
            ogg) build_cmake -DINSTALL_DOCS=OFF ;;
            vorbis) build_cmake ;;
            flac) build_cmake -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF \
                -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DWITH_OGG=ON ;;
            opus) build_cmake -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF ;;
            mpg123) build_autotools --disable-components --enable-libmpg123 --with-cpu=aarch64 ;;
            lame)
                # Upstream's legacy ID3 declarations need this with current Apple clang.
                # The workaround is intentionally confined to LAME's configure and build.
                # shellcheck disable=SC2030
                (export ac_cv_prog_cc_c23=no CFLAGS="$CFLAGS -Wno-implicit-function-declaration"
                 build_autotools --disable-frontend --disable-analyzer-hooks --with-fileio=lame)
                ;;
            sndfile) build_cmake -DBUILD_TESTING=OFF -DBUILD_PROGRAMS=OFF \
                -DBUILD_EXAMPLES=OFF -DENABLE_CPACK=OFF -DENABLE_EXTERNAL_LIBS=ON -DENABLE_MPEG=ON ;;
            lv2) build_meson -Dplugins=disabled ;;
            zix) build_meson -Dbenchmarks=disabled -Dtests_cpp=disabled ;;
            serd) build_meson -Dtools=disabled -Dman=disabled ;;
            sord) build_meson -Dtools=disabled -Dman=disabled ;;
            sratom) build_meson ;;
            lilv) build_meson -Dtools=disabled -Dbindings_py=disabled ;;
            suil) build_meson -Dcocoa=disabled -Dgtk2=disabled -Dgtk3=disabled \
                -Dqt5=disabled -Dqt6=disabled -Dx11=disabled ;;
        esac
    } >"$work/logs/$name.log" 2>&1 || {
        tail -n 60 "$work/logs/$name.log" >&2
        exit 1
    }
done <"$work/sources.tsv"

pkg-config --exists ogg vorbis vorbisenc flac opus libmpg123 sndfile lv2 zix-0 serd-0 sord-0 sratom-0 lilv-0 suil-0
[[ -f "$prefix/lib/cmake/SndFile/SndFileConfig.cmake" ]] || {
    echo "error: SndFileConfig.cmake is missing" >&2; exit 1;
}

cat >"$work/capabilities.c" <<'CAPABILITIES'
#include <sndfile.h>
#include <suil/suil.h>
#include <lv2/ui/ui.h>
#include <stdio.h>
int main(void)
{
    const int formats[] = { SF_FORMAT_FLAC | SF_FORMAT_PCM_16,
                            SF_FORMAT_OGG | SF_FORMAT_VORBIS,
                            SF_FORMAT_OGG | SF_FORMAT_OPUS,
                            SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III };
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        SF_INFO info = { 0 }; info.samplerate = 48000; info.channels = 2; info.format = formats[i];
        char path[64]; snprintf(path, sizeof(path), "codec-probe-%u.audio", i);
        SNDFILE* file = sf_open(path, SFM_WRITE, &info);
        if (!file) { fprintf(stderr, "cannot write audio format %x: %s\n", formats[i], sf_strerror(NULL)); return 1; }
        float samples[128];
        for (unsigned j = 0; j < 128; ++j) samples[j] = (j & 1) ? -0.125f : 0.25f;
        sf_count_t written = sf_writef_float(file, samples, 64);
        int closed = sf_close(file);
        remove(path);
        if (written != 64 || closed) { fprintf(stderr, "audio format %x write failed\n", formats[i]); return 1; }
        printf("Audio format %x: wrote 64 stereo frames\n", formats[i]);
    }
    /* suil reports identical host and UI types as directly embeddable however
       it was built, so this only proves suil links and answers the query. */
    if (!suil_ui_supported(LV2_UI__CocoaUI, LV2_UI__CocoaUI)) {
        fputs("suil rejected a Cocoa LV2 UI in a Cocoa host\n", stderr); return 1;
    }
    puts("FLAC/Vorbis/Opus/MPEG writing: PASS");
    puts("suil links and accepts a Cocoa LV2 UI in a Cocoa host (direct embed, no wrapper module)");
    return 0;
}
CAPABILITIES
# pkg-config emits compiler arguments, with only this prefix eligible for discovery.
# shellcheck disable=SC2046,SC2086,SC2031
"$CC" $CFLAGS "$work/capabilities.c" -o "$work/capabilities" \
    $(pkg-config --cflags --libs sndfile suil-0)
(cd "$work" && ./capabilities) | tee "$work/logs/capabilities.log"

python3 - "$prefix" "$work" "$sdk" <<'MANIFEST'
import csv, hashlib, json, re, subprocess, sys
from pathlib import Path
prefix, work, sdk = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
sources = [dict(zip(('name', 'version', 'url', 'sha256', 'license'), row))
           for row in csv.reader((work / 'sources.tsv').open(), delimiter='\t')]
libraries = []
for path in sorted((prefix / 'lib').glob('*.dylib')):
    if path.is_symlink():
        continue
    arch = subprocess.check_output(['lipo', '-archs', str(path)], text=True).strip()
    if arch != 'arm64':
        raise SystemExit('Unexpected architecture: ' + str(path) + ': ' + arch)
    commands = subprocess.check_output(['otool', '-l', str(path)], text=True)
    minimum = re.findall(r'^\s+minos\s+([0-9.]+)\s*$', commands, re.M)
    if not minimum:
        minimum = re.findall(r'cmd LC_VERSION_MIN_MACOSX\n\s+cmdsize \d+\n\s+version ([0-9.]+)', commands)
    if len(minimum) != 1 or tuple(map(int, minimum[0].split('.'))) > (11, 0, 0):
        raise SystemExit('Unsupported or missing deployment target: ' + str(path) + ': ' + str(minimum))
    dependencies = []
    for line in subprocess.check_output(['otool', '-L', str(path)], text=True).splitlines()[1:]:
        dependency = line.strip().split(' (compatibility version', 1)[0]
        if dependency.startswith(('/System/', '/usr/lib/')):
            continue
        resolved = Path(dependency).resolve()
        if not dependency.startswith(str(prefix) + '/') or not resolved.is_file() or prefix not in resolved.parents:
            raise SystemExit('Dependency outside the source-built prefix: ' + str(path) + ': ' + dependency)
        dependencies.append(dependency)
    libraries.append(dict(path=str(path.relative_to(prefix)), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                          architecture=arch, minimum_macos=minimum[0], dependencies=dependencies))
if not libraries:
    raise SystemExit('No source-built shared libraries were installed')
manifest = dict(architecture='arm64', deployment_target='11.0', sdk=sdk, sources=sources,
                libraries=libraries, capabilities='FLAC/Vorbis/Opus/MPEG writing; suil links and reports Cocoa-in-Cocoa LV2 UI embedding')
(prefix / 'runtime-deps-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('Verified %d source-built shared libraries targeting macOS 11 or earlier' % len(libraries))
MANIFEST
echo "Runtime dependencies installed in $prefix"
