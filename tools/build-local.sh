#!/usr/bin/env bash
set -euo pipefail

# build-local.sh: Fast local build using Docker llvm-mingw
# Compiles iphlpapi.dll in seconds without waiting for GitHub Actions runners.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE_HOST="${REMOTE_HOST:-fttg@192.168.178.32}"
REMOTE_SRC="${REMOTE_SRC:-/home/fttg/livemap-src}"
BUILD_DIR="${BUILD_DIR:-build-local}"
DEPLOY="${DEPLOY:-false}"

echo "==> Syncing sources to ${REMOTE_HOST}:${REMOTE_SRC}..."
rsync -az --exclude='.git' --exclude='build*' --exclude='.cache' "${SCRIPT_DIR}/" "${REMOTE_HOST}:${REMOTE_SRC}/"

echo "==> Building iphlpapi.dll with llvm-mingw..."
ssh "${REMOTE_HOST}" "docker run --rm -v ${REMOTE_SRC}:/src -w /src/${BUILD_DIR} mstorsjo/llvm-mingw:latest sh -c '
    if [ ! -f build.ninja ]; then
        cmake -DCMAKE_ASM_MASM_FLAGS=\"-m64\" -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release ..
    fi
    ninja iphlpapi
'"

if [ "${DEPLOY}" = "true" ]; then
    echo "==> Deploying to Enshrouded server..."
    ssh "${REMOTE_HOST}" "
        cp -f ${REMOTE_SRC}/${BUILD_DIR}/iphlpapi.dll /docker/appdata/enshrouded/game/iphlpapi.dll
        docker restart enshrouded
    "
    echo "==> Deployed and restarted enshrouded container."
fi

echo "==> Build complete: ${BUILD_DIR}/iphlpapi.dll"
