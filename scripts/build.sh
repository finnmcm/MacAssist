#!/usr/bin/env bash
# Configure (first run) and build the daemon + mactl (CMake), then the
# Swift UI app (SwiftPM). The two build systems are independent in Phase 1;
# they meet only over the socket protocol.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"
cmake --build build -j"$(sysctl -n hw.ncpu)"

swift build --package-path app -c "${SWIFT_CONFIG:-debug}"

echo
echo "built:"
echo "  build/daemon/macassistd"
echo "  build/tools/mactl/mactl"
echo "  app/.build/debug/MacAssist"
