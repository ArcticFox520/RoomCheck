#!/usr/bin/env bash
# RoomCheck test entry point
set -euo pipefail
cd "$(dirname "$0")/.."

make clean >/dev/null
make dev >/dev/null
python3 tests/test_api.py "$@"
