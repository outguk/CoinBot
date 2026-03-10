src/app : 엔트리 포인트, 콘솔 UI, 메인 루프

src/core : 공통 타입, 설정, 기본 인터페이스 (예: IOrderExecutor, IStrategy)

src/api : 업비트 REST / WebSocket 클라이언트

src/trading : 주문/포지션/리스크/계좌 관리

src/strategy : 전략 인터페이스와 개별 전략 구현

src/util : 로깅, 시간 유틸, 문자열 변환 등

include/coinbot/... : 외부에서 재사용 가능한 퍼블릭 헤더들 정리

## 최근 변경 사항

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
