#!/usr/bin/env bash
# Dev loop: configure + build the app and the test binary, run ctest.
# Canonical build dirs (build/ + build-tests/) per CLAUDE.md so macOS
# and Linux share state. ccache + compile_commands.json come from the
# top-level CMakeLists.
#
# Usage:
#   scripts/dev.sh            # app + tests + ctest (default)
#   scripts/dev.sh app        # configure + build app only
#   scripts/dev.sh tests      # configure + build tests + ctest
#   scripts/dev.sh selftest   # build app, then DUSKSTUDIO_RUN_SELFTEST=1
#
# Pass extra cmake args via CMAKE_ARGS env (e.g. -DJUCE_PATH=...).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

# Parallel job count, portable across macOS / Linux.
if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
  JOBS="$(sysctl -n hw.ncpu)"
else
  JOBS=4
fi

EXTRA_ARGS="${CMAKE_ARGS:-}"
TARGET="${1:-all}"

link_compile_commands() {
  # Point the repo-root symlink at whichever build dir was configured
  # most recently so clangd always resolves against fresh flags.
  local src="$1"
  if [ -f "${src}/compile_commands.json" ]; then
    ln -sf "${src}/compile_commands.json" "${REPO_ROOT}/compile_commands.json"
  fi
}

build_app() {
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ${EXTRA_ARGS}
  cmake --build build -j"${JOBS}"
  link_compile_commands build
}

build_tests() {
  cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release \
    -DDUSKSTUDIO_BUILD_TESTS=ON ${EXTRA_ARGS}
  cmake --build build-tests --target dusk-studio-tests -j"${JOBS}"
  ctest --test-dir build-tests --output-on-failure
}

case "${TARGET}" in
  app)      build_app ;;
  tests)    build_tests ;;
  selftest)
    build_app
    DUSKSTUDIO_RUN_SELFTEST=1 ./build/DuskStudio_artefacts/Release/DuskStudio
    ;;
  all)
    build_app
    build_tests
    ;;
  *)
    echo "usage: scripts/dev.sh [app|tests|selftest|all]" >&2
    exit 1
    ;;
esac

echo "dev.sh: ${TARGET} OK"
