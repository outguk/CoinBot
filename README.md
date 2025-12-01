src/app : 엔트리 포인트, 콘솔 UI, 메인 루프

src/core : 공통 타입, 설정, 기본 인터페이스 (예: IOrderExecutor, IStrategy)

src/api : 업비트 REST / WebSocket 클라이언트

src/trading : 주문/포지션/리스크/계좌 관리

src/strategy : 전략 인터페이스와 개별 전략 구현

src/util : 로깅, 시간 유틸, 문자열 변환 등

include/coinbot/... : 외부에서 재사용 가능한 퍼블릭 헤더들 정리
