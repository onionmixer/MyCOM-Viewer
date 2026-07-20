#!/usr/bin/env bash
set -euo pipefail

# Native Ubuntu/Debian build, CTest, staged install, and DEB package.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-linux"
STAGE_DIR="${PROJECT_DIR}/stage-linux"

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --parallel
ctest --test-dir "${BUILD_DIR}" --output-on-failure
cmake --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"
(
    cd "${BUILD_DIR}"
    cpack --config CPackConfig.cmake -G DEB
)

echo "Linux build, test, staged install, and DEB package completed."
