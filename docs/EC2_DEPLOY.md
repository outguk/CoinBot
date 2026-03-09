# EC2 배포 가이드

> 대상: AWS를 처음 접하는 입장에서 CoinBot을 EC2에 올리고 `ROADMAP 3.5`까지 맞추려는 경우
> 기준: Ubuntu 24.04 LTS, `t3.small`, 로컬 빌드(WSL2) + 바이너리 전송 방식
>
> [!info] 읽는 방법
> 이 문서는 "무엇을 왜 하는지"를 먼저 설명하고, 그 다음에 실제 명령과 AWS 콘솔 경로를 보여준다.
> 처음 읽을 때는 `빠른 용어 설명 -> 현재 실제 배포 순서 -> 각 상세 섹션` 순서로 보면 된다.

---

## 이 문서의 범위

이 문서는 아래 범위까지를 다룬다.

- `ROADMAP 3.2` 성격의 자동 재시작 전제 확인
  - 현재 바이너리는 WS 재연결 한도 초과/워커 비정상 종료 시 `std::exit(1)`로 종료되어 systemd `Restart=on-failure` 경로를 탄다.
- `ROADMAP 3.3` journald 운영
  - 로그는 파일이 아니라 `journalctl -u coinbot`으로 확인한다.
- `ROADMAP 3.4` 배포 스크립트 사용
  - `deploy/deploy.sh`, `deploy/coinbot.service`
- `ROADMAP 3.5` 인프라 구성
  - EC2
  - 별도 EBS 볼륨을 `/home/ubuntu/coinbot`에 마운트
  - S3 버킷 + IAM 역할
  - SQLite 일일 백업 cron

이 문서는 다루지 않는다.

- `ROADMAP 3.6` GracefulShutdown

---

## 빠른 용어 설명

처음 AWS를 접하면 이름은 비슷한데 역할은 다른 항목이 많다.
이 문서에서는 아래 의미로 사용한다.

| 용어 | 의미 | 왜 필요한가 |
|------|------|-------------|
| `EC2` | AWS 가상 서버 | CoinBot이 실제로 실행될 Ubuntu 서버 |
| `Elastic IP` | 고정 공인 IP | SSH 주소와 배포 대상 주소를 고정하기 위함 |
| `EBS` | EC2에 붙는 별도 디스크 | CoinBot, `.env`, SQLite DB를 루트 디스크와 분리하기 위함 |
| `IAM role` | EC2에 부여하는 AWS 권한 | S3 백업 업로드를 AWS 키 파일 없이 처리하기 위함 |
| `Security Group` | EC2 방화벽 | SSH 22번 포트를 내 IP에만 허용하기 위함 |
| `SSH key (.pem)` | EC2 로그인용 개인키 | 로컬 WSL에서 EC2로 접속할 때 사용 |
| `systemd service` | Linux 서비스 등록 방식 | CoinBot 자동 시작/자동 재시작 |
| `journald` | systemd 로그 저장소 | `journalctl -u coinbot`로 로그 확인 |
| `/etc/fstab` | 부팅 시 자동 마운트 설정 파일 | 재부팅 후에도 EBS 자동 마운트 |
| `sentinel file` | "이 경로가 올바른 EBS다"라는 표식 파일 | 잘못된 루트 디스크 배포 방지 |

> [!tip] 감으로 이해하기
> - `Elastic IP`: 서버의 고정 주소
> - `Security Group`: 누가 그 주소로 들어올 수 있는지 정하는 방화벽
> - `IAM role`: 그 서버가 AWS 안에서 무엇을 할 수 있는지 정하는 권한

---

## 어디서 하는 작업인가

배포 과정은 아래 3곳을 오간다.

| 작업 위치 | 예시 | 언제 쓰는가 |
|-----------|------|-------------|
| `AWS 콘솔` | EC2 생성, Elastic IP 연결, EBS attach, IAM role 연결 | AWS 리소스를 만들 때 |
| `로컬 WSL` | `ssh`, `~/.ssh/config`, `./deploy/deploy.sh` | 내 PC에서 EC2에 접속하거나 배포할 때 |
| `EC2 내부` | `lsblk`, `mkfs`, `mount`, `.env`, `journalctl` | 서버 안에서 디스크/환경을 준비할 때 |

> [!warning] 가장 자주 헷갈리는 점
> `Elastic IP`는 AWS 콘솔에서 만들지만,
> 그 IP로 접속하도록 `ssh`를 맞추는 작업은 로컬 WSL에서 한다.

---

## 현재 실제 배포 순서

지금 문서 기준으로, AWS 초보자가 가장 덜 헷갈리는 실제 진행 순서는 아래다.

| 순서 | 작업 | 위치 | 상세 섹션 |
|------|------|------|-----------|
| 1 | EC2 인스턴스 생성 | AWS 콘솔 | `2. EC2 인스턴스 생성` |
| 2 | Elastic IP 연결 | AWS 콘솔 | `2. EC2 인스턴스 생성 > Elastic IP 연결` |
| 3 | SSH 키 정리 후 SSH 접속 확인 | 로컬 WSL | `1. 사전 준비`, `3. SSH 접속 설정` |
| 4 | IAM 역할 생성 및 EC2 연결 | AWS 콘솔 | `5. S3 버킷과 IAM 역할 준비` |
| 5 | 별도 EBS 생성 및 attach | AWS 콘솔 | `4. EBS 볼륨 생성 및 마운트 > 4-1` |
| 6 | EBS 포맷 및 `/home/ubuntu/coinbot` 마운트 | EC2 내부 | `4-2`, `4-3` |
| 7 | `fstab` 등록으로 자동 마운트 설정 | EC2 내부 | `4-4` |
| 8 | `sentinel`, `db/`, 권한 정리 | EC2 내부 | `7. EC2 초기 설정` |
| 9 | `.env` 생성 | EC2 내부 | `8. API 키 설정` |
| 10 | `deploy.sh` 배포 | 로컬 WSL | `9. 배포 실행` |
| 11 | `journalctl`과 DB 파일 확인 | EC2 내부 | `10. 배포 확인` |
| 12 | S3 백업 cron 등록 | EC2 내부 | `11. S3 일일 백업 cron 설정` |

> [!warning] 순서상 가장 중요한 점
> `/home/ubuntu/coinbot`에 EBS를 먼저 마운트하고 나서 `.env`와 바이너리를 올린다.
> 마운트 전에 파일을 배포하면 그 파일은 루트 디스크에 써지고, 나중에 EBS를 마운트하면 가려진다.

---

## 전체 순서

처음 배포라면 아래 순서로 진행한다.

1. EC2 인스턴스를 생성한다.
2. Elastic IP를 연결한다.
3. 로컬 SSH 키를 정리하고 SSH 접속을 확인한다.
4. IAM 역할을 생성하고 EC2에 연결한다.
5. 별도 EBS 볼륨을 생성하고 EC2에 attach한다.
6. EC2 런타임 패키지를 설치한다.
7. EBS를 포맷하고 `/home/ubuntu/coinbot`에 마운트한다.
8. `/etc/fstab`에 자동 마운트 설정을 추가한다.
9. EBS 초기화 작업을 한다.
   - sentinel 파일 생성
   - `db/` 디렉토리 생성
10. EC2에 `.env`를 만든다.
11. 로컬 WSL2에서 `deploy.sh` preflight + 배포를 실행한다.
12. journald 로그와 DB 파일을 확인한다.
13. SQLite 백업 cron을 등록하고 S3 업로드를 검증한다.

---

## 1. 사전 준비

### 필요한 것

| 항목 | 설명 |
|------|------|
| AWS 계정 | EC2 / EBS / IAM / S3 생성 가능 계정 |
| SSH 키 페어 | EC2 접속용 `.pem` 파일 |
| 로컬 WSL2 | Ubuntu 24.04 권장 |
| Upbit API 키 | Access Key + Secret Key |

> 권장 버전을 24.04로 맞춘 이유:
> 현재 배포 방식은 로컬 Linux에서 빌드한 바이너리를 EC2로 전송하는 구조다.
> 로컬과 EC2를 같은 세대로 맞추는 편이 런타임 라이브러리 호환성에 유리하고,
> 현재 CMake는 `Boost 1.80+`를 요구하므로 Ubuntu 24.04 쪽이 더 자연스럽다.

### 로컬 SSH 키 준비

`.pem` 파일을 WSL2로 복사하고 권한을 제한한다.

```bash
mkdir -p ~/.ssh
cp /mnt/c/Users/<Windows사용자>/Downloads/coinbot-key.pem ~/.ssh/
chmod 400 ~/.ssh/coinbot-key.pem
```

> [!note] `~/.ssh`가 없다면
> WSL에서 `~`는 현재 사용자 홈 디렉토리다.
> 예를 들어 사용자명이 `inguk`이면 `~/.ssh`는 `/home/inguk/.ssh`를 뜻한다.

---

## 2. EC2 인스턴스 생성

AWS 콘솔에서 `EC2 -> Instances -> Launch instances`로 이동한다.

> [!info] 이 단계의 의미
> 여기서는 "빈 Ubuntu 서버 1대"를 만드는 것이다.
> 아직 CoinBot, DB, `.env`는 올라가지 않는다.

### 권장 설정

| 항목 | 설정값 |
|------|--------|
| Name | `coinbot` |
| AMI | `Ubuntu Server 24.04 LTS` |
| Instance type | `t3.small` |
| Key pair | 보유 키 페어 선택 또는 신규 생성 |
| Network | 기본 VPC 사용 가능 |
| Auto-assign public IP | Enable |
| Root volume | 16~20GB gp3 권장 |

### 보안 그룹

Inbound 규칙:

| Type | Port | Source | 이유 |
|------|------|--------|------|
| SSH | 22 | 내 공인 IP | 관리자 접속 |

Outbound 규칙:

| Type | Port | Destination | 이유 |
|------|------|-------------|------|
| All traffic | All | `0.0.0.0/0` | Upbit API, 패키지 설치, S3 백업 |

> CoinBot은 외부에서 접속받는 서버가 아니다.
> 따라서 인바운드는 SSH(22)만 열어두면 된다.

보안 그룹에서 자주 나오는 용어는 아래처럼 이해하면 된다.

| 용어 | 의미 |
|------|------|
| `Inbound` | 외부에서 EC2 안으로 들어오는 트래픽 |
| `Outbound` | EC2에서 외부로 나가는 트래픽 |
| `SSH` | 원격 Linux 서버에 안전하게 접속하는 표준 방식 |
| `TCP 22` | SSH가 기본적으로 사용하는 포트 번호 |
| `My IP` | 현재 내 인터넷 공인 IP 주소만 허용한다는 뜻 |

왜 `TCP`를 직접 고르지 않고 `SSH`를 고르냐면,
AWS 콘솔에서 `SSH`를 선택하면 내부적으로 `TCP 22` 규칙을 의미 있게 묶어서 보여주기 때문이다.
즉 기능적으로는 `TCP 22`와 같지만, 초보자 입장에서는 `SSH`가 목적이 더 분명하다.

현재 CoinBot 구조에서 이 설정이 맞는 이유:

- `Inbound`는 SSH만 있으면 충분하다.
  - CoinBot은 웹 서버가 아니므로 외부에서 80/443 같은 포트로 들어올 필요가 없다.
- `Outbound`는 전체 허용이 필요하다.
  - Upbit REST/WS 접속
  - `apt` 패키지 설치
  - S3 백업 업로드
  - 시간 동기화나 기타 시스템 통신

> [!warning] `SSH 22`를 `0.0.0.0/0`으로 열지 않는 이유
> 누구나 인터넷에서 해당 서버의 SSH 포트에 접근을 시도할 수 있기 때문이다.
> 처음 운영할 때는 최소한 `My IP`로 제한하는 편이 안전하다.

### Elastic IP 연결

퍼블릭 IP가 바뀌지 않게 하려면 Elastic IP를 바로 연결하는 편이 안전하다.

1. `EC2 -> Elastic IPs -> Allocate Elastic IP address`
2. 생성한 Elastic IP 선택
3. `Actions -> Associate Elastic IP address`
4. 방금 만든 `coinbot` 인스턴스에 연결

이후 SSH 설정에 이 Elastic IP를 사용한다.

> [!note] 왜 지금 연결하는가
> Elastic IP를 먼저 붙여두면 SSH 접속 주소와 이후 `deploy.sh` 대상 주소를 일찍 고정할 수 있다.
> 인스턴스의 임시 퍼블릭 IP를 기준으로 진행하면 나중에 주소를 다시 바꿔야 한다.

---

## 3. SSH 접속 설정

인스턴스의 Elastic IP가 정해진 뒤 `~/.ssh/config`를 등록한다.

> [!info] 이 단계는 로컬 WSL에서 진행한다
> `ssh` 명령과 `~/.ssh/config`는 내 PC의 설정이다.
> AWS 콘솔이 아니라 로컬 터미널에서 작업한다.

```text
Host coinbot
    HostName <Elastic_IP>
    User ubuntu
    IdentityFile ~/.ssh/coinbot-key.pem
```

접속 확인:

```bash
ssh coinbot
```

정상이라면 Ubuntu 쉘 프롬프트가 열린다.

`~/.ssh/config`를 쓰지 않는다면 아래처럼 직접 접속해도 된다.

```bash
ssh -i ~/.ssh/coinbot-key.pem ubuntu@<Elastic_IP>
```

---

## 4. EBS 볼륨 생성 및 마운트

`ROADMAP 3.5` 기준을 맞추려면 봇 데이터 경로 `/home/ubuntu/coinbot`를 별도 EBS에 올리는 편이 안전하다.

> [!info] 이 단계의 의미
> 루트 디스크는 Ubuntu 운영체제용으로 두고,
> CoinBot 실행 파일, `.env`, SQLite DB는 별도 EBS에 올려서 역할을 분리한다.

### 4-1. EBS 볼륨 생성

AWS 콘솔에서 `EC2 -> Volumes -> Create volume`.

| 항목 | 설정값 |
|------|--------|
| Volume type | `gp3` |
| Size | 20GB 이상 권장 |
| Availability Zone | EC2 인스턴스와 **같은 AZ** |
| Name tag | `coinbot-data` |

생성 후 `Actions -> Attach volume`으로 `coinbot` 인스턴스에 연결한다.

장치 이름 예시:

```text
/dev/sdf
```

> [!note] `Snapshot ID`는 보통 비워둔다
> 지금은 처음 만드는 빈 데이터 디스크이므로, 기존 스냅샷에서 복구할 내용이 없다.
> 따라서 새 빈 볼륨을 만든 뒤 EC2 안에서 직접 `ext4`로 포맷한다.

### 4-2. EC2에서 파일시스템 생성

EC2에 접속한 뒤 디스크 이름을 확인한다.

```bash
lsblk
```

보통 Ubuntu에서는 `/dev/nvme1n1`처럼 보일 수 있다.
새 볼륨 이름을 확인한 뒤 아래 명령을 실행한다.

```bash
sudo mkfs.ext4 /dev/<새_볼륨_디바이스명>
```

예시:

```bash
sudo mkfs.ext4 /dev/nvme1n1
```

> [!warning] 주의
> `mkfs.ext4`는 디스크 내용을 초기화한다.
> 반드시 `lsblk`로 새로 attach한 EBS가 맞는지 확인한 뒤 실행한다.

### 4-3. `/home/ubuntu/coinbot`에 마운트

```bash
sudo mkdir -p /home/ubuntu/coinbot
sudo mount /dev/<새_볼륨_디바이스명> /home/ubuntu/coinbot
sudo chown ubuntu:ubuntu /home/ubuntu/coinbot
df -h | grep coinbot
```

위 명령에서 `/home/ubuntu/coinbot`가 보이면 EBS가 해당 경로에 연결된 것이다.
`sentinel` 파일과 `db/` 디렉토리 생성은 뒤의 `7. EC2 초기 설정`에서 마무리한다.

### 4-4. 재부팅 후에도 자동 마운트되게 설정

UUID를 확인한다.

```bash
sudo blkid /dev/<새_볼륨_디바이스명>
```

출력 예시:

```text
/dev/nvme1n1: UUID="abcd-1234-..." TYPE="ext4"
```

`/etc/fstab`에 추가한다.

```bash
sudo cp /etc/fstab /etc/fstab.bak
sudo vi /etc/fstab
```

아래 한 줄 추가:

```text
UUID=<위에서_확인한_UUID> /home/ubuntu/coinbot ext4 defaults,nofail 0 2
```

문법 확인:

```bash
sudo umount /home/ubuntu/coinbot
sudo mount -a
df -h | grep coinbot
```

> `mount -a`가 오류 없이 끝나면 재부팅 후에도 자동 마운트될 가능성이 높다.

---

## 5. S3 버킷과 IAM 역할 준비

`ROADMAP 3.5`의 백업 요구사항은 EC2에서 S3로 DB 스냅샷을 올릴 수 있어야 충족된다.

> [!info] 이 단계의 의미
> `S3 버킷`은 백업 파일을 저장하는 장소이고,
> `IAM 역할`은 EC2가 그 버킷에 업로드할 수 있게 해주는 권한이다.
> 즉 "저장소"와 "접근 권한"을 같이 준비하는 단계다.

> [!note] 실제 진행 순서
> 처음 배포할 때는 이 IAM 역할 연결을 EBS 마운트 전에 해도 되고, 뒤에 해도 된다.
> 다만 `S3 백업 cron`까지 한 번에 끝내려면 배포 전에 연결해 두는 편이 덜 헷갈린다.

### 5-1. S3 버킷 생성

AWS 콘솔에서 `S3 -> Create bucket`.

예시:

| 항목 | 값 예시 |
|------|---------|
| Bucket name | `coinbot-backup-<고유문자열>` |
| Region | EC2와 같은 리전 권장 |
| Block Public Access | 기본값 유지 |

### 5-2. IAM 역할 생성

AWS 콘솔에서 `IAM -> Roles -> Create role`.

설정:

1. Trusted entity type: `AWS service`
2. Use case: `EC2`
3. 권한 정책: S3 버킷에만 접근 가능한 최소 권한 정책 연결

정책 예시:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "s3:PutObject",
        "s3:GetObject",
        "s3:ListBucket"
      ],
      "Resource": [
        "arn:aws:s3:::coinbot-backup-<고유문자열>",
        "arn:aws:s3:::coinbot-backup-<고유문자열>/*"
      ]
    }
  ]
}
```

역할 이름 예시:

```text
coinbot-ec2-backup-role
```

### 5-3. EC2에 IAM 역할 연결

`EC2 -> Instances -> coinbot -> Actions -> Security -> Modify IAM role`

방금 만든 역할을 연결한다.

연결 후 EC2에서 확인:

```bash
aws sts get-caller-identity
```

> 이 명령은 뒤에서 `awscli` 설치 후 실행한다.

---

## 6. 로컬 WSL2 빌드 환경 준비

로컬 WSL2에서 최초 1회 설치한다.

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libboost-all-dev \
    libssl-dev \
    nlohmann-json3-dev
```

빌드 확인:

```bash
cd ~/CoinBot
cmake --preset linux-release
cmake --build out/build/linux-release -j$(nproc)
ls -lh out/build/linux-release/CoinBot
```

---

## 7. EC2 초기 설정

EC2에서 최초 1회만 진행한다.

```bash
ssh coinbot
```

### 런타임 패키지 설치

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    git \
    cmake \
    libboost-all-dev \
    libssl-dev \
    nlohmann-json3-dev \
    sqlite3 \
    awscli
```

설치 이유:

- `deploy.sh`가 EC2 쪽 기본 패키지 설치를 수행한다.
- 하지만 S3 백업 검증까지 하려면 `awscli`가 필요하다.
- `sqlite3`는 `.backup` 명령에 필요하다.

### EBS 운영 경로 초기화

`/home/ubuntu/coinbot`가 EBS에 마운트된 상태라는 전제에서,
CoinBot 운영에 필요한 기본 구조를 만든다.

```bash
echo coinbot-ebs > /home/ubuntu/coinbot/.coinbot_volume_sentinel
mkdir -p /home/ubuntu/coinbot/db
ls -ld /home/ubuntu/coinbot
ls -ld /home/ubuntu/coinbot/db
ls -l /home/ubuntu/coinbot/.coinbot_volume_sentinel
```

정상 기대값:

- `/home/ubuntu/coinbot`가 EBS 마운트 경로다.
- `/home/ubuntu/coinbot/db`가 존재한다.
- `/home/ubuntu/coinbot/.coinbot_volume_sentinel` 파일이 존재한다.

각 항목의 의미:

- `db/`: SQLite DB가 저장될 디렉토리
- `.coinbot_volume_sentinel`: 이 경로가 진짜 EBS라는 표식 파일
- 위 둘이 없으면 `deploy.sh`와 `coinbot.service`가 시작 전에 중단된다.

---

## 8. API 키 설정

`.env`는 **EC2에서 직접 작성**한다.

```bash
cat > /home/ubuntu/coinbot/.env << 'EOF'
UPBIT_ACCESS_KEY=여기에_실제_Access_Key_입력
UPBIT_SECRET_KEY=여기에_실제_Secret_Key_입력
UPBIT_MARKETS=KRW-XRP,KRW-ADA,KRW-TRX
EOF

chmod 600 /home/ubuntu/coinbot/.env
```

변수 설명:

| 변수 | 설명 |
|------|------|
| `UPBIT_ACCESS_KEY` | Upbit API Access Key |
| `UPBIT_SECRET_KEY` | Upbit API Secret Key |
| `UPBIT_MARKETS` | CSV 형식 거래 마켓 목록 |

---

## 9. 배포 실행

배포 전에 아래 두 파일의 역할을 먼저 이해하면 전체 흐름이 덜 헷갈린다.

| 파일 | 역할 |
|------|------|
| `deploy/deploy.sh` | 로컬 WSL2에서 실행하는 배포 스크립트. 로컬 빌드, EC2 경로 검증, 바이너리 전송, systemd 서비스 재시작까지 한 번에 수행한다. |
| `deploy/coinbot.service` | EC2의 systemd 서비스 정의 파일. CoinBot을 `/home/ubuntu/coinbot`에서 실행하고, `.env`를 읽고, 비정상 종료 시 자동 재시작하게 만든다. |

즉, 역할 분담은 아래와 같다.

- `deploy.sh`: "배포하는 도구"
- `coinbot.service`: "배포된 봇을 EC2에서 계속 실행시키는 규칙"

처음 배포할 때는 로컬에서 `deploy.sh`를 실행하고,
그 스크립트가 내부적으로 `coinbot.service`를 EC2에 등록한다고 이해하면 된다.

즉, 아래 단계는 `deploy.sh`가 한 번에 처리한다.

- EC2 경로 preflight 확인
- 로컬 빌드
- 바이너리 전송
- `coinbot.service` 배치/갱신
- `systemctl daemon-reload`
- `systemctl enable coinbot`
- `systemctl restart coinbot`

### 9-1. `deploy.sh` 접속 대상 설정

로컬 WSL2에서 `deploy/deploy.sh` 상단의 `EC2_HOST`를 수정한다.

```bash
vi ~/CoinBot/deploy/deploy.sh
```

권장:

```bash
EC2_HOST="coinbot"
```

또는 직접 IP 사용:

```bash
EC2_HOST="ubuntu@<Elastic_IP>"
```

### 9-2. 배포 스크립트 실행

```bash
cd ~/CoinBot
chmod +x deploy/deploy.sh
./deploy/deploy.sh
```

수행 내용:

1. EC2 패키지 설치
2. 로컬 `linux-release` 빌드
3. 원격 `/home/ubuntu/coinbot/db` 보장
4. 바이너리 전송
5. systemd 서비스 등록 및 재시작

정상 완료 시 `systemctl status` 결과에 `Active: active (running)`이 보여야 한다.

---

## 10. 배포 확인

### 서비스 상태

```bash
ssh coinbot
sudo systemctl status coinbot
```

### 실시간 로그

```bash
sudo journalctl -u coinbot -f
```

정상 로그 예시:

```text
[CoinBot] Database opened: db/coinbot.db
[CoinBot] Initializing MarketEngineManager...
[CoinBot] Live candle type: candle.15m
[CoinBot] Starting...
[CoinBot] Running. Press Ctrl+C to stop.
```

### DB 파일 생성 확인

```bash
ls -lh /home/ubuntu/coinbot/db/coinbot.db
```

### 자동 재시작 설정 확인

```bash
sudo systemctl cat coinbot
```

확인 포인트:

- `WorkingDirectory=/home/ubuntu/coinbot`
- `EnvironmentFile=/home/ubuntu/coinbot/.env`
- `Restart=on-failure`

의미:

- `WorkingDirectory`: CoinBot이 기준으로 삼는 현재 작업 폴더
- `EnvironmentFile`: Upbit API 키를 `.env`에서 읽도록 지정
- `Restart=on-failure`: WS fatal이나 예외 종료 시 systemd가 자동으로 다시 실행

여기서 보이는 내용이 바로 `deploy/coinbot.service`가 EC2에 등록된 결과다.

---

## 11. S3 일일 백업 cron 설정

`ROADMAP 3.5` 기준의 핵심은 SQLite DB를 `.backup`으로 안전하게 스냅샷 떠서 S3에 올리는 것이다.

### 11-1. IAM 역할 동작 확인

EC2에서:

```bash
aws sts get-caller-identity
aws s3 ls s3://<버킷이름>
```

둘 다 정상 동작해야 한다.

### 11-2. 백업 스크립트 등록

EC2에서 아래 스크립트를 만든다.

```bash
cat > /home/ubuntu/coinbot/backup_db.sh << 'EOF'
#!/bin/bash
set -e

TODAY=$(date +%F)
SRC_DB="/home/ubuntu/coinbot/db/coinbot.db"
SNAPSHOT="/tmp/coinbot_snapshot.db"
BUCKET="s3://<버킷이름>/$TODAY/coinbot.db"

sqlite3 "$SRC_DB" ".backup $SNAPSHOT"
aws s3 cp "$SNAPSHOT" "$BUCKET"
rm -f "$SNAPSHOT"
EOF

chmod 700 /home/ubuntu/coinbot/backup_db.sh
```

### 11-3. 수동 실행 테스트

```bash
/home/ubuntu/coinbot/backup_db.sh
aws s3 ls s3://<버킷이름>/$(date +%F)/
```

### 11-4. cron 등록

```bash
crontab -e
```

매일 00:00 실행 예시:

```cron
0 0 * * * /home/ubuntu/coinbot/backup_db.sh >> /home/ubuntu/coinbot/backup_db.log 2>&1
```

> `backup_db.log`는 백업 검증 편의를 위한 로그다.
> CoinBot 본체 로그 운영은 계속 journald 기준으로 본다.

---

## 12. 이후 재배포

코드를 수정한 뒤에는 로컬에서 아래만 다시 실행하면 된다.

```bash
cd ~/CoinBot
./deploy/deploy.sh
```

유지되는 항목:

- `/home/ubuntu/coinbot/.env`
- `/home/ubuntu/coinbot/db/coinbot.db`
- cron 설정

---

## 13. 운영 명령 모음

```bash
# 서비스 상태
sudo systemctl status coinbot

# 실시간 로그
sudo journalctl -u coinbot -f

# 최근 100줄
sudo journalctl -u coinbot -n 100

# 오늘 로그
sudo journalctl -u coinbot --since today

# 재시작
sudo systemctl restart coinbot

# 중단
sudo systemctl stop coinbot

# 부팅 자동 시작 해제
sudo systemctl disable coinbot

# 백업 수동 실행
/home/ubuntu/coinbot/backup_db.sh
```

---

## 14. 트러블슈팅

### `Permission denied (publickey)`

로컬 WSL2에서:

```bash
ls -la ~/.ssh/coinbot-key.pem
chmod 400 ~/.ssh/coinbot-key.pem
```

### `scp: /home/ubuntu/coinbot/CoinBot: Permission denied`

EC2에서 마운트/권한을 다시 확인한다.

```bash
df -h | grep coinbot
ls -ld /home/ubuntu/coinbot
sudo chown ubuntu:ubuntu /home/ubuntu/coinbot
```

### 배포 시작 직후 `[0/5] EC2 경로 검증...` 에서 실패

대부분 아래 셋 중 하나다.

- `/home/ubuntu/coinbot`가 EBS 마운트포인트가 아님
- `/home/ubuntu/coinbot/.coinbot_volume_sentinel` 파일이 없음
- `/home/ubuntu/coinbot/db` 디렉토리가 없음

확인:

```bash
mountpoint /home/ubuntu/coinbot
ls -la /home/ubuntu/coinbot
df -h | grep coinbot
```

### `[DB] open 실패`

`db/` 디렉토리가 없거나 EBS 마운트가 풀린 경우다.

```bash
df -h | grep coinbot
mkdir -p /home/ubuntu/coinbot/db
sudo systemctl restart coinbot
```

### `환경 변수가 없습니다: UPBIT_ACCESS_KEY`

`.env`를 확인한다.

```bash
ls -la /home/ubuntu/coinbot/.env
cat /home/ubuntu/coinbot/.env
chmod 600 /home/ubuntu/coinbot/.env
```

### `Active: failed`

원인 로그 확인:

```bash
sudo journalctl -u coinbot -n 100 --no-pager
```

### `aws s3 cp`가 AccessDenied로 실패

대부분 아래 둘 중 하나다.

- EC2에 IAM 역할이 연결되지 않음
- IAM 정책의 버킷 ARN이 실제 버킷 이름과 다름

확인:

```bash
aws sts get-caller-identity
aws s3 ls s3://<버킷이름>
```

### 재부팅 후 `/home/ubuntu/coinbot`가 비어 있음

`/etc/fstab` 또는 EBS 마운트가 잘못된 경우다.

```bash
cat /etc/fstab
lsblk
df -h | grep coinbot
sudo mount -a
```

---

## 관련 문서

- [ROADMAP.md](ROADMAP.md) — Phase 3 운영 목표와 완료 기준
- [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) — 현재 구현 범위 확인
