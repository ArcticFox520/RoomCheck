#!/usr/bin/env bash
# Build fully static musl binaries.
# Usage: scripts/build-musl.sh [amd64|arm64|all]
set -euo pipefail
cd "$(dirname "$0")/.."

TARGET="${1:-all}"
build_one() {
  local arch="$1" cc="$2" outdir="$3"
  if ! command -v "$cc" >/dev/null 2>&1; then
    echo "缺少编译器 $cc，无法构建 $arch" >&2
    return 1
  fi
  echo "== 构建 $arch ($cc) =="
  make clean >/dev/null
  make CC="$cc" LDFLAGS="-static -s -no-pie" build/RoomCheck
  mkdir -p "$outdir"
  cp build/RoomCheck "$outdir/RoomCheck"
  scripts/verify-static.sh "$outdir/RoomCheck" "$arch" --run
}

case "$TARGET" in
  amd64) build_one amd64 musl-gcc dist/linux-amd64-musl ;;
  arm64) build_one arm64 aarch64-linux-musl-gcc dist/linux-arm64-musl ;;
  all)
    build_one amd64 musl-gcc dist/linux-amd64-musl
    build_one arm64 aarch64-linux-musl-gcc dist/linux-arm64-musl
    ;;
  *) echo "用法: $0 [amd64|arm64|all]" >&2; exit 2 ;;
esac
