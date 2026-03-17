src/app : 엔트리 포인트, 콘솔 UI, 메인 루프

src/core : 공통 타입, 설정, 기본 인터페이스 (예: IOrderExecutor, IStrategy)

src/api : 업비트 REST / WebSocket 클라이언트

src/trading : 주문/포지션/리스크/계좌 관리

src/strategy : 전략 인터페이스와 개별 전략 구현

src/util : 로깅, 시간 유틸, 문자열 변환 등

include/coinbot/... : 외부에서 재사용 가능한 퍼블릭 헤더들 정리

## 최근 변경 사항

### 2026-03-17

- `PositionEffect` 기반 주문 상태 전이를 도입했다.
  - 기존에는 `Filled`/`Canceled` + 내부 상태로 포지션 변화를 추론했다. 잘못된 상태 전이(Issue 1: Canceled+전량체결 → 항상 InPosition, Issue 2: WS 체결 유실 시 SELL volume=0)가 발생할 수 있었다.
  - `core::PositionEffect` 열거형(`None`/`Opened`/`Reduced`/`Closed`)을 추가하고, `MarketEngine::resolvePositionEffect_()`에서 finalize 이후 계좌 잔고 기준으로 effect를 확정해 이벤트에 실어 전략에 전달한다.
  - `RsiMeanReversionStrategy::onOrderUpdate()`는 effect만 보고 상태 전이를 결정한다. 상태와 체결 여부를 조합하던 이전 추론 로직은 제거됐다.
  - `EngineOrderStatusEvent`와 `trading::OrderStatusEvent` 모두에 `position_effect` 필드가 추가됐다
- `Config.h` 중복 임계값을 정리했다.
  - `StrategyConfig::dust_exit_threshold_krw`를 제거하고 전략 코드 전체에서 `min_notional_krw`로 통합했다.
  - 두 값이 항상 동일하다는 제약을 각 필드 주석에 명시했다.

### 2026-03-16

- Private WS EOF 문제를 해결했다.
  - Upbit private WebSocket은 주문 이벤트가 없으면 서버가 약 120s 후 앱 레벨 idle 판정으로 EOF를 보낸다.
  - `HeartbeatMode::UpbitTextPing` 모드를 추가하고, `ws_private`에 30s 주기로 `"PING"` 텍스트 프레임을 전송하도록 설정했다.
  - 서버 응답 `{"status":"UP"}`는 JSON 파싱으로 정확히 확인한 뒤 `on_msg_` 호출 전에 필터링해 엔진 계층에 노출되지 않도록 했다.
  - 기존 컨트롤 ping(25s)은 NAT 무음 드롭 등 죽은 TCP 감지 목적으로 유지했다 — 두 keepalive는 역할이 다르다.
  - `ws_public`은 초당 캔들 수신이 자연 keepalive 역할을 하므로 미적용.
- `UpbitWebSocketClient` 코드를 정리했다.
  - 컨트롤 ping과 텍스트 PING 블록을 단일 keepalive 블록으로 통합했다.
  - `connectImpl` 탭/스페이스 혼용 및 주석 번호 스타일을 통일했다.
  - ping `if(!ec)` / `if(ec)` → `if/else` 정리, 미사용 `#include <iostream>` 제거, 타이머 `now()` 중복 호출 통합, `exp` → `backoff_exp` 이름 변경.

### 2026-03-12

- 주문 복구 경로를 단순화했다.
  - `MarketEngineManager`는 pending 주문 복구 시 `getOrder()` 재시도만 사용하고, `getOpenOrders()` fallback 경로는 제거했다.
  - incomplete snapshot 판정은 `MarketEngine::reconcileFromSnapshot()` 쪽에 맡겨 복구 책임을 한 곳으로 모았다.
- `ReservationToken` move assignment를 제거했다.
  - `MarketEngine`의 활성 매수 토큰 보관은 대입 대신 `std::optional::emplace()`를 사용한다.
  - 토큰 소멸자 경로만 남았으므로 `releaseWithoutToken()` 설명도 현재 구조에 맞게 정리했다.
- DB/로컬 파일 관리 기준을 정리했다.
  - `Database::open()`은 SQLite가 DB 파일이 없을 때 새 파일을 생성하고, 열기 직후 임베디드 스키마와 PRAGMA를 적용한다.
  - `docs/*.md`와 vendored `src/database/sqlite3.c/.h`는 로컬 보관 대상으로 `.gitignore`에 추가했다.

### 2026-03-11

- Intrabar 손절/익절 청산 로직을 추가했다.
  - 기존에는 확정 분봉 종가 기준으로만 손절/익절을 판단했다.
  - `RsiMeanReversionStrategy::onIntrabarCandle()`을 추가해, InPosition 상태에서 미확정 캔들의 close가 손절가 이하 또는 익절가 이상에 도달하면 즉시 시장가 매도를 제출한다.
  - RSI 기반 청산은 확정 종가 전용으로 유지되며, intrabar 경로는 stop/target 체크만 수행한다.
  - `MarketEngineManager::handleMarketData_()` 내부에 `doIntrabarCheck` 헬퍼를 추가하고, 실시간 캔들 수신의 세 경로(첫 수신·동일 ts 업데이트·새 분봉)에서 모두 호출한다.
  - mark price는 submit 성공/실패 여부와 무관하게 intrabar close로 항상 최신화된다.
  - DB signals 기록 시 rsi/volatility/trend_strength는 NULL로 저장되며, exit_reason은 `exit_stop` / `exit_target` / `exit_stop_target` 규칙을 그대로 따른다.

### 2026-03-10

- AWS 초보자 기준의 EC2 배포 문서를 정리했다.
  - `docs/EC2_DEPLOY.md`에 `Elastic IP`, `IAM role`, `EBS`, `fstab`, `sentinel file` 설명과 실제 배포 순서를 반영했다.
  - Ubuntu 24.04 / `t3.small` / 로컬 WSL 빌드 + EC2 배포 흐름으로 문서를 맞췄다.
- 배포 안전 장치를 추가했다.
  - `deploy/deploy.sh`와 `deploy/coinbot.service`가 `/home/ubuntu/coinbot` 마운트포인트와 sentinel 파일을 검사하도록 정리했다.
  - EBS가 마운트되지 않은 상태에서 루트 디스크로 잘못 배포되는 상황을 방지한다.
- 저장소 정리를 진행했다.
  - 로컬 전용 파일과 생성 산출물이 Git에 다시 올라가지 않도록 `.gitignore`를 보강했다.
  - `build/`, `.claude/settings.local.json`, `CLAUDE.md`, `.mcp.json`은 로컬 보관 기준으로 추적에서 제외했다.

## 로컬 실행

로컬 WSL에서 Upbit 키를 매번 직접 입력하지 않으려면 프로젝트 루트에 `.env.local`을 두고 실행 스크립트를 사용한다.

1. 예시 파일 복사

```bash
cp .env.local.example .env.local
```

2. 실제 키 입력

```bash
nano .env.local
```

3. 로컬 실행

```bash
bash scripts/run_local.sh
```

기본 실행 파일 경로는 `out/build/linux-release/CoinBot`이며, 다른 바이너리를 실행하려면 첫 번째 인자로 경로를 넘기면 된다.
