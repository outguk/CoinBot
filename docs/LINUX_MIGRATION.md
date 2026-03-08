# Linux 마이그레이션 가이드

> EC2 Ubuntu 배포를 위한 Windows → Linux 크로스플랫폼 전환 기록

---

## 배경

CoinBot은 원래 Windows/MSVC 전용으로 개발됐다. EC2 Ubuntu 배포를 위해 CMake 설정과
일부 소스 코드를 크로스플랫폼으로 수정했다. 소스 코드 대부분(`#ifdef _WIN32` 가드,
Boost.Beast, std::thread 등)은 원래부터 크로스플랫폼으로 작성되어 있어 CMake 설정과
MSVC 전용 함수 일부만 수정하면 됐다.

---

## 수정 파일 목록

| 파일 | 변경 내용 |
|------|----------|
| `CMakeLists.txt` | 의존성 경로 `if(WIN32)` 분기, nlohmann include 경로 |
| `tests/CMakeLists.txt` | `PLATFORM_INCLUDE_DIRS` 변수로 경로 통합 |
| `CMakePresets.json` | Linux 프리셋 추가 |
| `src/db/Database.cpp` | `sscanf_s` → `sscanf`, `_mkgmtime` → `#ifdef` 분기 |
| `src/util/Config.h` | `db_path` Windows 절대경로 → 상대경로 |
| `src/app/CoinBot.cpp` | `market_logs` 경로 → 상대경로 |
| `deploy/coinbot.service` | systemd 서비스 파일 (신규) |
| `deploy/deploy.sh` | EC2 배포 스크립트 (신규) |
| `deploy/.env.template` | 환경변수 템플릿 (신규) |

---

## 1. CMakeLists.txt — 의존성 경로 분기

Windows는 하드코딩 경로, Linux는 시스템 패키지를 사용하도록 분기했다.

```cmake
if(WIN32)
    set(BOOST_ROOT "C:/git-repository/boost_1_89_0")
    set(BOOST_LIBRARYDIR "${BOOST_ROOT}/stage/lib")
    set(Boost_NO_SYSTEM_PATHS ON)
    set(Boost_USE_STATIC_LIBS ON)
    set(OPENSSL_ROOT_DIR "C:/git-repository/OpenSSL-Win64")
    set(OPENSSL_USE_STATIC_LIBS OFF)
else()
    set(Boost_USE_STATIC_LIBS OFF)
endif()
```

include 경로도 분기:

```cmake
if(WIN32)
    target_include_directories(CoinBot PRIVATE
        "C:/git-repository/boost_1_89_0"
        "C:/git-repository/nlohmann_json"
    )
else()
    # apt 설치 시 헤더 위치: /usr/include/nlohmann/json.hpp
    # 코드가 <json.hpp>로 include하므로 nlohmann 디렉토리를 경로에 추가
    target_include_directories(CoinBot PRIVATE /usr/include/nlohmann)
endif()
```

---

## 2. tests/CMakeLists.txt — PLATFORM_INCLUDE_DIRS

각 테스트 타겟마다 반복되던 하드코딩 경로를 공통 변수로 통합했다.

```cmake
if(WIN32)
    set(PLATFORM_INCLUDE_DIRS
        "C:/git-repository/boost_1_89_0"
        "C:/git-repository/nlohmann_json"
    )
else()
    set(PLATFORM_INCLUDE_DIRS "")
endif()

# 각 타겟에서 재사용
target_include_directories(test_xxx PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PLATFORM_INCLUDE_DIRS}
)
```

---

## 3. CMakePresets.json — Linux 프리셋 추가

```json
{
    "name": "linux-base",
    "hidden": true,
    "generator": "Unix Makefiles",
    "binaryDir": "${sourceDir}/out/build/${presetName}",
    "installDir": "${sourceDir}/out/install/${presetName}",
    "condition": {
        "type": "equals",
        "lhs": "${hostSystemName}",
        "rhs": "Linux"
    }
},
{
    "name": "linux-debug",
    "displayName": "Linux Debug",
    "inherits": "linux-base",
    "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" }
},
{
    "name": "linux-release",
    "displayName": "Linux Release",
    "inherits": "linux-base",
    "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" }
}
```

---

## 4. Database.cpp — MSVC 전용 함수 교체

### sscanf_s → sscanf

`sscanf_s`는 MSVC 전용. `sscanf`는 표준 C 함수로 양쪽 모두 동작한다.

```cpp
// 변경 전
sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec);

// 변경 후
sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec);
```

### _mkgmtime → timegm (#ifdef 분기)

`_mkgmtime`은 MSVC 전용, POSIX 대응은 `timegm`.

```cpp
#ifdef _WIN32
    const int64_t epoch_sec = static_cast<int64_t>(_mkgmtime(&tm));
#else
    const int64_t epoch_sec = static_cast<int64_t>(timegm(&tm));
#endif
```

---

## 5. 하드코딩 경로 → 상대경로

systemd의 `WorkingDirectory`가 CWD를 고정해주므로 상대경로로 변경했다.

```cpp
// Config.h — db_path
std::string db_path = "coinbot.db";  // WorkingDirectory 기준

// CoinBot.cpp — 로그 경로
logger.enableMarketFileOutput("market_logs");
```

EC2 배포 디렉토리 구조:
```
/home/ubuntu/coinbot/
├── CoinBot          ← 바이너리
├── coinbot.db       ← DB (자동 생성)
├── market_logs/     ← 로그 (자동 생성)
└── .env             ← API 키
```

---

## 6. 원래부터 크로스플랫폼이었던 코드

수정 불필요했던 항목들:

| 파일 | 내용 |
|------|------|
| `Logger.h` | `localtime_s` / `localtime_r` — `#ifdef _WIN32` 가드 존재 |
| `CoinBot.cpp` | `_dupenv_s` / `std::getenv` — `#ifdef _WIN32` 가드 존재 |
| `UpbitWebSocketClient` | Boost.Beast — 크로스플랫폼 |
| `UpbitJwtSigner` | OpenSSL — 크로스플랫폼 |
| 스레딩 전반 | `std::thread`, `std::jthread` — 표준 라이브러리 |
| `sqlite3.c` | SQLite amalgamation — 자체 플랫폼 분기 처리 |

---

## 7. deploy/ 파일

### coinbot.service (systemd)

```ini
[Unit]
Description=CoinBot - Upbit Trading Bot
After=network-online.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/home/ubuntu/coinbot
ExecStart=/home/ubuntu/coinbot/CoinBot
EnvironmentFile=/home/ubuntu/coinbot/.env
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

### .env.template

```bash
UPBIT_ACCESS_KEY=여기에_Access_Key_입력
UPBIT_SECRET_KEY=여기에_Secret_Key_입력
UPBIT_MARKETS=KRW-XRP,KRW-ADA,KRW-TRX  # 생략 시 Config.h 기본값 사용
```

---

## 8. Phase 3 미완료 항목 (배포 후 작업)

현재 빌드/실행은 가능하나 운영 안정성을 위해 추가 작업이 필요한 항목들이다.
ROADMAP.md Phase 3에 상세 계획이 있다.

| 항목 | 내용 | ROADMAP |
|------|------|---------|
| WS 헬스체크 | give_up 후 프로세스 살아있어 systemd Restart 미발동 | 3.3 |
| Graceful Shutdown | 미체결 주문 정리 없이 종료 | 3.2 |
| DB 백업 | SQLite WAL 온라인 백업 + S3 | 3.6 |
| 로그 관리 | journald 용량 제한, 로그 회전 | 3.4 |

> WS 헬스체크와 Graceful Shutdown은 C++ 코드 수정이므로 Windows/Linux 동일하게 동작한다.
> DB 백업과 로그 관리는 플랫폼별 도구가 다르다 (Linux: cron/logrotate, Windows: 작업 스케줄러).

---

## 9. WSL2에서 빌드 확인 방법

EC2 배포 전 로컬 Windows에서 Ubuntu 빌드를 검증하는 방법이다.

### 환경

- WSL2 + Ubuntu 22.04 또는 24.04
- `/mnt/c/cpp/CoinBot`은 권한 문제로 빌드 불가 → WSL 내부로 복사 필요

### 의존성 설치 (최초 1회)

```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-all-dev libssl-dev nlohmann-json3-dev sqlite3
```

### 빌드

```bash
# Windows 소스를 WSL 내부로 복사 (코드 수정 후 매번)
rm -rf ~/Coinbot && cp -r /mnt/c/cpp/CoinBot ~/Coinbot
cd ~/Coinbot

# 이전 빌드 캐시 제거
rm -rf out

cmake --preset linux-release
cmake --build out/build/linux-release -j$(nproc)
```

### 주의사항

- 코드 편집은 항상 `C:\cpp\CoinBot`(Windows VS)에서 한다
- Linux 빌드 확인 시마다 WSL로 재복사가 필요하다
- EC2 최종 배포 시에는 `git clone`으로 EC2 내부에 받아 빌드한다

---

## 10. EC2 배포 흐름 (향후)

```bash
# EC2 의존성 설치 (최초 1회)
sudo apt install -y build-essential cmake libboost-all-dev libssl-dev nlohmann-json3-dev sqlite3

# API 키 설정
cp deploy/.env.template /home/ubuntu/coinbot/.env
vi /home/ubuntu/coinbot/.env
chmod 600 /home/ubuntu/coinbot/.env

# 배포 스크립트 실행 (로컬에서)
./deploy/deploy.sh
```

또는 `deploy/deploy.sh`가 의존성 설치부터 서비스 등록까지 자동으로 처리한다.
