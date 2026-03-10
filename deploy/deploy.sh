#!/bin/bash
# EC2 배포 스크립트 (로컬에서 실행: ./deploy/deploy.sh)
# 사전 조건: EC2에 SSH 키 설정, BINARY 빌드 완료
#
# 이 스크립트는 "항상 /home/ubuntu/coinbot 이 EBS 데이터 볼륨"이라는 전제를 둔다.
# 그래서 배포 시작 전에 mountpoint + sentinel 파일을 먼저 확인한다.
# 이 검증이 없으면, EBS가 빠진 상태에서도 같은 경로명이 존재하기만 하면
# 바이너리와 DB가 루트 디스크에 잘못 써질 수 있다.

set -e

EC2_HOST="coinbot"   # ← EC2 IP로 변경
REMOTE_DIR="/home/ubuntu/coinbot"
BINARY="out/build/linux-release/CoinBot"
SENTINEL_FILE="$REMOTE_DIR/.coinbot_volume_sentinel"

echo "=== CoinBot EC2 배포 ==="

# EBS 마운트/경로 검증
# - mountpoint -q:
#   /home/ubuntu/coinbot 가 "그냥 폴더"가 아니라 실제 마운트된 볼륨인지 확인
# - sentinel 파일:
#   운영자가 의도한 데이터 볼륨이라는 표식
#   (실수로 다른 디스크/빈 폴더에 배포되는 것을 한 번 더 방지)
# - db 디렉토리:
#   SQLite 상대 경로(db/coinbot.db)가 즉시 유효한지 확인
#
# 셋 중 하나라도 실패하면 배포를 중단한다.
# "배포가 안 되는 것"이 "잘못된 디스크에 조용히 배포되는 것"보다 안전하다.
echo "[0/5] EC2 경로 검증..."
ssh "$EC2_HOST" "
    mountpoint -q $REMOTE_DIR &&
    test -f $SENTINEL_FILE &&
    test -d $REMOTE_DIR/db
"

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
ssh "$EC2_HOST" "mkdir -p $REMOTE_DIR/db"

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
