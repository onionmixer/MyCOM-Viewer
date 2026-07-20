#!/usr/bin/env bash
set -euo pipefail

# Native macOS build, CTest, staged installation, and PKG package.
# Override QT_ROOT when a different Qt5 kit is installed.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
QT_ROOT="${QT_ROOT:-${HOME}/opt/Qt/5.15.2/clang_64}"
BUILD_DIR="${PROJECT_DIR}/build-macos"
STAGE_DIR="${PROJECT_DIR}/stage-macos"

# The supported user-local provisioning path installs CMake beside aqtinstall.
if [[ -d "${HOME}/.local/mycom-aqt/bin" ]]; then
    PATH="${HOME}/.local/mycom-aqt/bin:${PATH}"
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "CMake was not found. Install it into ${HOME}/.local/mycom-aqt/bin or set PATH." >&2
    exit 1
fi

if [[ ! -f "${QT_ROOT}/lib/cmake/Qt5/Qt5Config.cmake" ]]; then
    echo "Qt5 CMake package was not found below: ${QT_ROOT}" >&2
    exit 1
fi

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${QT_ROOT}"
cmake --build "${BUILD_DIR}" --parallel
DYLD_LIBRARY_PATH="${QT_ROOT}/lib${DYLD_LIBRARY_PATH:+:${DYLD_LIBRARY_PATH}}" \
    ctest --test-dir "${BUILD_DIR}" --output-on-failure

# A staging prefix can retain files from an older install layout.  Recreate
# only disposable staging/package output so the PKG contains this build alone.
cmake -E rm -rf "${STAGE_DIR}" "${BUILD_DIR}/packages"
cmake -E make_directory "${STAGE_DIR}"
cmake --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"
(
    cd "${BUILD_DIR}"
    cpack --config CPackConfig.cmake -G productbuild
)

echo "macOS build, test, staged installation, and PKG package completed."
