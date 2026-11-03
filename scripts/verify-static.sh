#!/usr/bin/env bash
# Verify that a RoomCheck binary is a fully static Linux ELF for the given arch.
# Usage: scripts/verify-static.sh <binary> <amd64|arm64> [--run]
set -euo pipefail

BIN="${1:?用法: verify-static.sh <binary> <amd64|arm64> [--run]}"
ARCH="${2:?缺少架构参数}"
RUN_TEST="${3:-}"

if [[ ! -f "$BIN" ]]; then
  echo "FAIL: 找不到文件 $BIN" >&2
  exit 1
fi

echo "== 验证静态二进制: $BIN ($ARCH) =="
FILE_OUT="$(file -b "$BIN")"
echo "file: $FILE_OUT"
[[ "$FILE_OUT" == *ELF* ]] || { echo "FAIL: 不是 ELF 文件" >&2; exit 1; }
[[ "$FILE_OUT" == *"statically linked"* ]] || { echo "FAIL: 不是静态链接" >&2; exit 1; }
case "$ARCH" in
  amd64) [[ "$FILE_OUT" == *x86-64* || "$FILE_OUT" == *"x86_64"* ]] || { echo "FAIL: 架构不是 amd64" >&2; exit 1; } ;;
  arm64) [[ "$FILE_OUT" == *aarch64* || "$FILE_OUT" == *AArch64* ]] || { echo "FAIL: 架构不是 arm64" >&2; exit 1; } ;;
  *) echo "FAIL: 未知架构 $ARCH" >&2; exit 1 ;;
esac

if readelf -l "$BIN" | grep -q "INTERP"; then
  echo "FAIL: 存在动态加载器 INTERP" >&2
  exit 1
fi
if readelf -d "$BIN" 2>/dev/null | grep -q "NEEDED"; then
  echo "FAIL: 存在动态依赖 NEEDED" >&2
  exit 1
fi
if ldd "$BIN" 2>&1 | grep -Eq "=>|libc\.so"; then
  echo "FAIL: ldd 显示动态依赖" >&2
  exit 1
fi

HOST_ARCH="$(uname -m)"
SHOULD_RUN=0
if [[ "$ARCH" == "amd64" && "$HOST_ARCH" =~ ^(x86_64|amd64)$ ]]; then
  SHOULD_RUN=1
fi
if [[ "$ARCH" == "arm64" && "$HOST_ARCH" =~ ^(aarch64|arm64)$ ]]; then
  SHOULD_RUN=1
fi
if [[ "$RUN_TEST" == "--run" && "$SHOULD_RUN" != "1" ]]; then
  echo "注意: 请求运行验证但宿主架构 $HOST_ARCH 与目标 $ARCH 不符，已跳过"
fi
RUNNER=""
if [[ "$SHOULD_RUN" != "1" && "$ARCH" == "arm64" ]] && command -v qemu-aarch64-static >/dev/null 2>&1; then
  RUNNER="qemu-aarch64-static"
  SHOULD_RUN=1
fi
if [[ "$SHOULD_RUN" == "1" ]]; then
  if [[ -n "$RUNNER" ]]; then
    echo "通过 $RUNNER 运行目标架构自检"
    "$RUNNER" "$BIN" --version
    "$RUNNER" "$BIN" --selftest
  else
    "$BIN" --version
    "$BIN" --selftest
  fi
else
  echo "跳过运行验证（宿主架构 $HOST_ARCH 与目标 $ARCH 不同）"
fi

echo "SHA256: $(sha256sum "$BIN" | awk '{print $1}')"
echo "SIZE:   $(stat -c%s "$BIN") bytes"
echo "OK: 静态链接验证通过"
