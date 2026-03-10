#!/usr/bin/env bash
# 로컬 WSL 실행용 스크립트
# - 프로젝트 루트의 .env.local을 읽어 환경 변수를 주입한다.
# - 키를 매번 셸에 직접 입력하지 않고 동일한 방식으로 실행하기 위한 목적이다.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="$REPO_ROOT/.env.local"
DEFAULT_BINARY="$REPO_ROOT/out/build/linux-release/CoinBot"
BINARY_PATH="${1:-$DEFAULT_BINARY}"

if [[ ! -f "$ENV_FILE" ]]; then
    echo "[run_local] .env.local 파일이 없습니다: $ENV_FILE"
    echo "[run_local] .env.local.example을 복사해서 실제 키를 입력하세요."
    exit 1
fi

if [[ ! -x "$BINARY_PATH" ]]; then
    echo "[run_local] 실행 파일이 없거나 실행 권한이 없습니다: $BINARY_PATH"
    exit 1
fi

set -a
source "$ENV_FILE"
set +a

: "${UPBIT_ACCESS_KEY:?UPBIT_ACCESS_KEY가 .env.local에 없습니다.}"
: "${UPBIT_SECRET_KEY:?UPBIT_SECRET_KEY가 .env.local에 없습니다.}"

cd "$REPO_ROOT"
exec "$BINARY_PATH"
