# 소스 폴더 구조
# src/
#   app/        -> main 함수, CLI, 엔트리포인트
#   core/       -> 공용 인터페이스, 도메인 모델
#   api/        -> 업비트 REST/WebSocket
#   trading/    -> 주문/포지션/리스크 관리
#   strategy/   -> 전략 인터페이스/구현
#   util/       -> 로깅, 유틸리티

# 라이브러리 타깃: cointbot_core (엔진 / 로직 모음)
file(GLOB_RECURSE COINBOT_CORE_SOURCES
	core/*.cpp
	api/*.cpp
	trading/*.cpp
	strategy/*.cpp
	util/*.cpp
)