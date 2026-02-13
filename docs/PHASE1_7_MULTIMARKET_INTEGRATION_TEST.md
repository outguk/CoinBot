# Phase 1.7 멀티마켓 통합 테스트 가이드

## 목적
- `ROADMAP 1.7 멀티마켓 테스트`를 실제 실행 순서로 진행하고, Phase 2 진입 가능 여부를 판단한다.

## 범위
- 대상 실행 파일: `CoinBot` (`src/app/CoinBot.cpp`)
- 참고: `tests/real_trading_test.cpp`는 현재 실행 경로가 주석 처리되어 있으므로, 통합 실행 기준은 `CoinBot`으로 본다.

## 사전 준비
1. API 키 환경 변수 설정
   - `UPBIT_ACCESS_KEY`
   - `UPBIT_SECRET_KEY`
2. 빌드 도구 확인
   - Visual Studio 2022 + CMake + Ninja
3. 리스크 통제
   - 소액 계정으로 시작
   - 초기에는 마켓 1개만 운영

## 실행 순서
1. 선행 단위/컴포넌트 테스트 확인
   - `test_account_manager_unified`
   - `test_market_engine`
   - `test_event_router`
   - `test_market_engine_manager`
2. 통합 스모크 테스트 (1마켓, 10~30분)
   - `src/app/CoinBot.cpp`의 `markets`를 1개로 설정
   - 프로그램 실행 후 종료 없이 안정 동작 확인
3. 통합 확장 테스트 (2마켓, 30~60분)
   - 이벤트 라우팅 분리, 주문 처리, 워커 생존 확인
4. 멀티마켓 부하 테스트 (3~5마켓, 1시간)
   - 장시간 실행 중 메모리 증가 추세/지연/오류 로그 확인

## 실행 명령 예시
```powershell
# Configure
cmake --preset x64-debug

# Build
cmake --build out/build/x64-debug --target CoinBot

# Run
.\out\build\x64-debug\CoinBot.exe
```

## 1.7 게이트 판정 기준
### 기능 검증 (필수)
1. 3개 이상 마켓 동시 실행 가능
2. 마켓별 독립 KRW 할당 정상 동작
3. WS 이벤트가 올바른 마켓으로 라우팅됨
4. 부분 체결 누적/평단 계산이 정확함
5. 재시작 시 마켓별 포지션 복구가 동작함
6. AccountManager와 실제 계좌 동기화(초기 1차/2차) 동작 확인

### 성능 검증 (권장)
1. 5마켓 1시간 연속 운영
2. CPU/메모리 안정
3. EventRouter fallback 비율 5% 미만
4. 주문 제출 지연 p95 500ms 미만

## 증적(로그) 수집 권장
1. 실행 시작/종료 시각
2. 마켓별 주문 제출 성공/실패 건수
3. 라우팅 실패(unknown market, parse failure) 건수
4. 동기화/복구 단계 로그
5. 장애 또는 예외 발생 시 원인 로그

## 완료 체크리스트
- [ ] 1마켓 스모크 통과
- [ ] 2마켓 통합 통과
- [ ] 3마켓 이상 동시 실행 통과
- [ ] 재시작 복구 시나리오 통과
- [ ] 1시간 부하 테스트 통과
- [ ] 1.7 기능 6개 항목 모두 확인
