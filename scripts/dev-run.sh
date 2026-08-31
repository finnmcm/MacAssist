#!/usr/bin/env bash
# Run the daemon in the foreground on throwaway dev paths, so you can
# Ctrl-C it and poke at it with:  mactl --socket /tmp/macassist-dev.sock ...
#
# Usage: scripts/dev-run.sh [SOCKET] [extra macassistd args...]
#   e.g. scripts/dev-run.sh /tmp/macassist-dev.sock --root "$HOME/Desktop"
set -euo pipefail
cd "$(dirname "$0")/.."

SOCK="${1:-/tmp/macassist-dev.sock}"
shift || true

exec ./build/daemon/macassistd \
  --socket "$SOCK" \
  --db "${MACASSIST_DB:-/tmp/macassist-dev.db}" \
  "$@"
