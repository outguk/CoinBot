#!/bin/bash
# EC2 배포 스크립트 (로컬에서 실행: ./deploy/deploy.sh)
# 사전 조건: EC2에 SSH 키 설정, BINARY 빌드 완료

set -e

EC2_HOST="ubuntu@<EC2_PUBLIC_IP>"   # ← EC2 IP로 변경
REMOTE_DIR="/home/ubuntu/coinbot"
BINARY="out/build/linux-release/CoinBot"

echo "=== CoinBot EC2 배포 ==="

# 1. EC2 의존성 설치 (최초 1회, 이미 설치된 경우 빠르게 skip됨)
echo "[1/5] EC2 의존성 설치..."
ssh "$EC2_HOST" "
    sudo apt-get update -q
    sudo apt-get install -y build-essential git cmake \
        libboost-all-dev libssl-dev nlohmann-json3-dev sqlite3
"

# 2. 빌드
echo "[2/5] 빌드 중..."
cmake --preset linux-release
cmake --build out/build/linux-release -j$(nproc)

# 3. 원격 디렉토리 준비
echo "[3/5] EC2 디렉토리 준비..."
ssh "$EC2_HOST" "mkdir -p $REMOTE_DIR/market_logs"

# 4. 바이너리 전송
echo "[4/5] 바이너리 전송..."
scp "$BINARY" "$EC2_HOST:$REMOTE_DIR/CoinBot"

# 5. 서비스 등록 및 재시작
echo "[5/5] 서비스 재시작..."
scp deploy/coinbot.service "$EC2_HOST:/tmp/coinbot.service"
ssh "$EC2_HOST" "
    sudo mv /tmp/coinbot.service /etc/systemd/system/coinbot.service
    sudo systemctl daemon-reload
    sudo systemctl enable coinbot
    sudo systemctl restart coinbot
    sudo systemctl status coinbot --no-pager
"

echo "=== 배포 완료 ==="
echo "로그 확인: ssh $EC2_HOST 'journalctl -u coinbot -f'"
