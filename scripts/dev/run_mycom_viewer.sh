#!/usr/bin/env bash
set -euo pipefail

# Development helper for Linux/macOS shells. This is not a distributed launcher.
# Usage: scripts/dev/run_mycom_viewer.sh [ISO_PATH] [ARCHIVE_DIRECTORY]

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
ISO_PATH="${1:-${MYCOM_ISO:-}}"
ARCHIVE_DIR="${2:-${PROJECT_DIR}/mycom_archive}"
BUILD_DIR="${MYCOM_BUILD_DIR:-${PROJECT_DIR}/build}"

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --parallel

if [[ ! -f "${ARCHIVE_DIR}/manifest.json" ]]; then
    if [[ -z "${ISO_PATH}" || ! -f "${ISO_PATH}" ]]; then
        echo "Archive manifest is absent. Supply ISO_PATH or set MYCOM_ISO." >&2
        exit 1
    fi
    "${BUILD_DIR}/mycom-archive-build" "${ISO_PATH}" "${ARCHIVE_DIR}"
fi

exec "${BUILD_DIR}/mycom-viewer" "${ARCHIVE_DIR}"
