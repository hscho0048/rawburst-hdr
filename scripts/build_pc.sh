#!/usr/bin/env bash
# PC(x64) 빌드. Windows면 WSL2 Ubuntu에서 실행: wsl -d Ubuntu -- bash scripts/build_pc.sh
set -e
cd "$(dirname "$0")/.."
cmake -S . -B build-pc -DCMAKE_BUILD_TYPE=Release
cmake --build build-pc -j
