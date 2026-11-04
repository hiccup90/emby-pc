#!/usr/bin/env bash
# Linux 环境下的编译/链接验证脚本（适用于无 Windows、无 GPU 的容器或 CI）。
#
# 只验证代码能编译链接，不做运行验证：--wid 嵌入、D3D11 硬解、
# HDR/DV tone-map、mpv OSC 等仍需在 Windows 实机上测试。
#
# 用法：
#   ./scripts/build-linux.sh
#   BUILD_DIR=build-check BUILD_TYPE=Release ./scripts/build-linux.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${APP_DIR}/build-linux}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

echo "==> cmake configure: ${APP_DIR} -> ${BUILD_DIR} (${BUILD_TYPE})"
cmake -S "${APP_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "==> cmake build"
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo "==> OK: ${BUILD_DIR}/emby-pc"
