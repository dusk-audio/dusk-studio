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
#   scripts/dev.sh selftest   # build app, then self-test under private Xvfb
#
# Pass extra cmake args via CMAKE_ARGS env (e.g. -DJUCE_PATH=...). Override the
# conservative parallel-build limit with DUSK_JOBS only when the host has room.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

# Keep Makefiles and Ninja explicitly bounded: bare -j is unbounded with Make,
# and host CPU counts ignore the 350-500 MB peak of large translation units.
JOBS="${DUSK_JOBS:-6}"
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: DUSK_JOBS must be a positive integer" >&2
  exit 2
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
    scripts/run-selftest-xvfb.sh
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
