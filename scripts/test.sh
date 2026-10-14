#!/usr/bin/env bash
# Build and run the daemon test suite (Catch2 + CTest). Configures if needed,
# builds the daemon_tests target, then runs it through CTest.
#
# Pass extra args through to the test binary for a filtered run, e.g.:
#   ./scripts/test.sh "[indexer]"        # one tag
#   ./scripts/test.sh -l                 # list all test cases
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" >/dev/null
cmake --build build -j"$(sysctl -n hw.ncpu)" --target daemon_tests

if [ "$#" -gt 0 ]; then
  exec ./build/daemon/tests/daemon_tests "$@"
fi
ctest --test-dir build --output-on-failure
