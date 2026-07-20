#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="/mnt/STORAGE16T/mycom/mycom_mvb_converter"
ISO_PATH="/mnt/STORAGE16T/mycom/MYCOM_145_extracted/MYCOM.ISO"
ARCHIVE_DIR="${PROJECT_DIR}/mycom_archive"
BUILD_DIR="${PROJECT_DIR}/build"
ARCHIVE_BUILD="${BUILD_DIR}/mycom-archive-build"
VIEWER="${BUILD_DIR}/mycom-viewer"

if [[ ! -f "${ISO_PATH}" ]]; then
    echo "MYCOM ISO was not found: ${ISO_PATH}" >&2
    exit 1
fi

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j2

if [[ ! -f "${ARCHIVE_DIR}/manifest.json" ]]; then
    "${ARCHIVE_BUILD}" "${ISO_PATH}" "${ARCHIVE_DIR}"
fi

exec "${VIEWER}" "${ARCHIVE_DIR}"
