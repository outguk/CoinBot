# CoinBot 로드맵 (미래 계획)

> **현재 구조**: [ARCHITECTURE.md](ARCHITECTURE.md)
> **구현 현황**: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md)
> 최종 갱신: 2026-02-19

---

## 전체 진행 상태

```
Phase 0  [완료] 기존 코드 리팩토링
Phase 1  [완료] 멀티마켓 핵심 구현
Phase 1.7 [진행중] 장시간 부하/안정화 검증
Phase 2  [미시작] PostgreSQL 거래 기록
Phase 3  [미시작] AWS 24시간 운영
```

---

## Phase 1.7: 장시간 부하/안정화 검증

**전제**: Phase 1 기능 경로 구현 완료
**목적**: 운영 환경에서의 안정성 검증

### 필수 게이트 (Phase 2 진입 전)

- [ ] 장시간 부하 테스트 통과 (최소 1시간 연속 실행)
- [ ] `unknown_funds` 재시도 시나리오 검증
- [ ] pending 장기 고착 대응 정책 확정

### 권장 (Phase 2와 병행 가능)

- [ ] emergency sync (`rebuildFromAccount` 조건부 호출) 구현
- [ ] 큐 포화 관측 지표 강화

### 알려진 리스크

1. **unknown_funds 반복 시 pending 장기 유지**
   - 현재: 보류 + 재시도
   - 필요: 조건부 emergency sync

2. **큐 포화 시 이벤트 유실 가능성 (drop-oldest)**
   - 현재 저빈도(1분봉, 소수 마켓) 조건에서는 가능성 낮음
   - 처리량 증가 시 myOrder 전용 큐 분리 필요

3. **봇 외부 거래와 로컬 상태 불일치**
   - 현재 정책: 외부 주문 체결은 무시
   - 운영 전제: 봇 단독 계좌 사용

---

## Phase 2: PostgreSQL 거래 기록

**전제 조건**: Phase 1.7 필수 게이트 통과

### 구현 태스크

```
2.1 스키마 설계 (0.5일)
    - sql/schema.sql 작성
    - trades, orders, strategy_snapshots, performance_daily 테이블
    - CHECK 제약 조건 포함

2.2 DatabasePool (1일)
    - libpqxx 연결 풀 (RAII Connection 가드)
    - 타임아웃 처리

2.3 TradeLogger (3일)
    - BoundedQueue(10,000) + 배치 쓰기 (100개 or 5초마다 플러시)
    - DB 장애 시 로컬 WAL 폴백 (JSON Lines, COINBOT_WAL_DIR 환경변수)
    - 재시작 시 WAL → DB 복구
    - 백프레셔 메트릭 (total_queued, wal_fallback_count, db_write_failures)

2.4 기존 코드 통합 (1일)
    - MarketEngine에서 TradeLogger 호출
    - 전략 스냅샷 저장

2.5 복구 테스트 (1일)
    - WAL 복구 시나리오
    - DB 재연결 테스트
    - 백프레셔 동작 확인
```

### 스키마 개요

```sql
-- 거래 기록
CREATE TABLE trades (
    id              BIGSERIAL PRIMARY KEY,
    trade_id        VARCHAR(64) NOT NULL,
    order_id        VARCHAR(64) NOT NULL,
    market          VARCHAR(20) NOT NULL,
    position        VARCHAR(10) NOT NULL CHECK (position IN ('BID', 'ASK')),
    price           DECIMAL(20, 8) NOT NULL,
    volume          DECIMAL(20, 8) NOT NULL,
    fee             DECIMAL(20, 8) NOT NULL DEFAULT 0,
    executed_at     TIMESTAMPTZ NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_trades_trade_id UNIQUE (trade_id)
);

-- 주문 기록 (시장가 매수 시 order_amount_krw 사용, 나머지는 order_volume)
CREATE TABLE orders (
    id                  BIGSERIAL PRIMARY KEY,
    order_id            VARCHAR(64) NOT NULL,
    market              VARCHAR(20) NOT NULL,
    position            VARCHAR(10) NOT NULL CHECK (position IN ('BID', 'ASK')),
    order_type          VARCHAR(20) NOT NULL,
    price               DECIMAL(20, 8),
    order_volume        DECIMAL(20, 8),
    order_amount_krw    DECIMAL(20, 8),
    executed_volume     DECIMAL(20, 8) NOT NULL DEFAULT 0,
    status              VARCHAR(20) NOT NULL,
    created_at          TIMESTAMPTZ NOT NULL,
    CONSTRAINT uq_orders_order_id UNIQUE (order_id),
    CONSTRAINT chk_order_size CHECK (
        (order_volume IS NOT NULL AND order_amount_krw IS NULL) OR
        (order_volume IS NULL AND order_amount_krw IS NOT NULL)
    )
);

-- 전략 상태 스냅샷 (재시작 복구용)
CREATE TABLE strategy_snapshots (
    id              BIGSERIAL PRIMARY KEY,
    market          VARCHAR(20) NOT NULL,
    state           VARCHAR(20) NOT NULL,
    entry_price     DECIMAL(20, 8),
    stop_price      DECIMAL(20, 8),
    target_price    DECIMAL(20, 8),
    position_volume DECIMAL(20, 8),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- 일별 성과 요약
CREATE TABLE performance_daily (
    id              BIGSERIAL PRIMARY KEY,
    date            DATE NOT NULL,
    market          VARCHAR(20) NOT NULL,
    total_trades    INT NOT NULL DEFAULT 0,
    total_pnl       DECIMAL(20, 8) NOT NULL DEFAULT 0,
    total_fees      DECIMAL(20, 8) NOT NULL DEFAULT 0,
    CONSTRAINT uq_performance_date_market UNIQUE (date, market)
);
```

### Phase 2 완료 기준

- [ ] 모든 거래가 DB에 기록됨
- [ ] DB 장애 시 WAL로 폴백
- [ ] 재시작 시 WAL에서 복구
- [ ] 백프레셔 메트릭 정상 동작

---

## Phase 3: AWS 24시간 운영

**전제 조건**: Phase 2 완료 기준 충족

### 구현 태스크

```
3.1 SignalHandler (1일)
    - SIGTERM/SIGINT 처리
    - stop_flag 전파

3.2 GracefulShutdown (2일)
    - 시장가 주문 체결 대기 (최대 30초)
    - REST API로 주문 상태 확인
    - 미체결 지정가 주문 정책 적용 (Cancel / KeepOpen)
    - TradeLogger flush (최대 30초, 타임아웃 시 WAL 저장)
    - 종료 요약 로그

3.3 HealthChecker (1일)
    - WS 연결 상태
    - DB 연결 상태
    - 마켓 스레드 상태
    - TradeLogger 백프레셔

3.4 Logger 개선 (1일)
    - 구조화된 JSON 로깅
    - CloudWatch 싱크 (선택적)
    - 로그 레벨 동적 변경

3.5 배포 스크립트 (1일)
    - Dockerfile 작성
    - coinbot.service (systemd)
    - 환경 변수 템플릿

3.6 인프라 구성 (2일)
    - EC2 인스턴스 설정
    - RDS PostgreSQL 설정
    - CloudWatch 알림 설정
    - Secrets Manager 연동
```

### AWS 배포 구성

```
EC2 Instance (t3.small 이상)
  └─ Systemd coinbot.service
       └─ CoinBot Process
            ├─ MarketEngineManager
            ├─ WebSocket Clients
            └─ MarketEngine × N
                   │
                   ├──► RDS PostgreSQL (거래 기록)
                   └──► Secrets Manager (API 키, DB 정보)

CloudWatch: 로그 스트리밍 + 메트릭 수집
```

### systemd 서비스 (예시)

```ini
[Unit]
Description=CoinBot Multi-Market Trading Service
After=network.target

[Service]
Type=simple
User=coinbot
WorkingDirectory=/opt/coinbot
ExecStart=/opt/coinbot/bin/CoinBot
Restart=always
RestartSec=10
EnvironmentFile=/etc/coinbot/env
MemoryMax=512M
TimeoutStopSec=90
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

### Phase 3 완료 기준

- [ ] systemd로 자동 재시작
- [ ] SIGTERM 시 graceful shutdown
- [ ] 진행 중 주문 안전하게 처리
- [ ] CloudWatch에서 메트릭 확인 가능
- [ ] 24시간 무중단 테스트 통과

---

## 의존성 흐름

```
Phase 0 [완료] → Phase 1 [완료] → Phase 1.7 [진행] → Phase 2 → Phase 3
```
