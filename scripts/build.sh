#!/usr/bin/env bash
# Configure (first run) and build the daemon + mactl.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"
cmake --build build -j"$(sysctl -n hw.ncpu)"

echo
echo "built:"
echo "  build/daemon/macassistd"
echo "  build/tools/mactl/mactl"
