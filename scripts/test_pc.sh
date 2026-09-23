#!/usr/bin/env bash
# 전체 단위 테스트 + 에뮬레이터 수용 테스트 (WSL: wsl -d Ubuntu -- bash scripts/test_pc.sh)
set -e
cd "$(dirname "$0")/.."
bash scripts/build_pc.sh > /dev/null
ctest --test-dir build-pc --output-on-failure "$@"
