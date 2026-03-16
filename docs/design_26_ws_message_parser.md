# #26 WS/REST 매핑 책임 계층 불일치 — 1단계: MarketEngineManager 파싱 위임

## Context

review.md #26 (P4): REST 경로는 `UpbitExchangeRestClient` 내부에서 JSON→DTO→Domain 변환을 완전히 캡슐화해 호출자는 도메인 객체만 받는다. 반면 WS 경로는 `EngineInput`에 raw JSON 문자열을 담아 app 계층인 `MarketEngineManager::handleMyOrder_()`와 `handleMarketData_()`에서 직접 JSON 파싱 → DTO 변환 → 도메인 매핑을 수행한다. 결과적으로 `MarketEngineManager.cpp`가 `api::upbit::dto`, `api::upbit::mappers`, `nlohmann::json` 세 가지 API 내부 구현에 직접 의존한다.

**이번 작업 범위(1단계)**: `WsMessageParser` 파사드를 API 계층에 추가해 `MarketEngineManager`가 DTO/mapper/JSON 헤더를 직접 include하지 않도록 의존성을 정리한다. `EngineInput` 구조(raw JSON 보관)와 `EventRouter` 구조(라우팅 키 추출)는 변경하지 않는다.

**미해결(2단계)**: `EngineInput`이 여전히 `std::string json`을 보유하므로 EventRouter → 큐 → MarketEngineManager 경계에는 raw JSON이 남는다. 완전 해소는 `EngineInput`을 도메인 타입으로 교체하는 별도 작업이 필요하며, IO 스레드 파싱 비용 및 EventRouter 변경을 수반한다.

---

## 현재 의존 구조 (문제)

```
MarketEngineManager.cpp (app 계층)
  ├─ #include <json.hpp>                          ← API 구현 세부 사항
  ├─ #include "api/upbit/mappers/MyOrderMapper.h" ← API 구현 세부 사항
  └─ #include "api/upbit/mappers/CandleMapper.h"  ← API 구현 세부 사항

handleMyOrder_():
  nlohmann::json::parse(raw.json)        // 0) JSON 파싱
  j.get<UpbitMyOrderDto>()               // 1) DTO 변환
  api::upbit::mappers::toEvents(dto)     // 2) 도메인 매핑

handleMarketData_():
  nlohmann::json::parse(raw.json)        // 0) JSON 파싱
  j.value("type","") → type check        // 1) 타입 확인
  parseMinuteCandleUnit(type)            // 2) 분봉 단위 추출 (anonymous ns 로컬 헬퍼)
  j.get<CandleDto_Minute>()              // 3) DTO 변환
  api::upbit::mappers::toDomain(dto)     // 4) 도메인 매핑
```

## 목표 의존 구조 (수정 후)

```
MarketEngineManager.cpp (app 계층)
  └─ #include "api/upbit/WsMessageParser.h"  ← core 타입만 노출하는 파사드

handleMyOrder_():
  ws_parser_.parseMyOrder(raw.json, ctx.market)  // 파싱 전체 위임 (empty = 실패)

handleMarketData_():
  ws_parser_.parseCandle(raw.json, configured_unit, ctx.market)  // 파싱 전체 위임 (nullopt = 실패 or non-candle)
```

---

## 변경 파일 목록

| 파일 | 유형 | 내용 |
|------|------|------|
| `src/api/upbit/WsMessageParser.h` | **신규** | `WsOrderEvent`, `WsCandleResult`, `WsMessageParser` 클래스 선언 |
| `src/api/upbit/WsMessageParser.cpp` | **신규** | `parseMyOrder()`, `parseCandle()` 구현; `parseMinuteCandleUnit()` 이전 |
| `src/api/CMakeLists.txt` | 수정 | `upbit/WsMessageParser.cpp` 소스 추가 |
| `src/app/MarketEngineManager.h` | 수정 | `#include "api/upbit/WsMessageParser.h"`, `ws_parser_` 멤버 추가 |
| `src/app/MarketEngineManager.cpp` | 수정 | `#include <json.hpp>`, 두 mapper include 제거; 파싱 로직 파사드 호출로 교체; `parseMinuteCandleUnit()` 제거 |

---

## 상세 설계

### 1. `src/api/upbit/WsMessageParser.h` (신규)

```cpp
#pragma once

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "core/domain/Candle.h"
#include "core/domain/MyTrade.h"
#include "core/domain/Order.h"

namespace api::upbit {

    // MyOrderMapper::MyOrderEvent와 동일한 underlying 타입
    // app 계층이 mappers 헤더를 직접 include하지 않도록 여기서 재선언
    using WsOrderEvent = std::variant<core::Order, core::MyTrade>;

    struct WsCandleResult {
        core::Candle candle;
        int unit_minutes;   // 메시지 타입에서 추출 or configured_fallback_unit 사용
    };

    class WsMessageParser {
    public:
        // raw WS myOrder JSON → 이벤트 목록
        // market: 로그 문맥용 (에러 로그에 마켓명 포함)
        // 파싱 실패 시 empty 반환 (내부에서 logger.error 기록)
        std::vector<WsOrderEvent> parseMyOrder(
            std::string_view json, std::string_view market = "");

        // raw WS candle JSON → Candle + unit
        // non-candle 메시지 또는 파싱 실패 시 nullopt
        // non-candle은 정상 경로이므로 silent, 파싱 실패는 logger.error 기록
        // configured_fallback_unit: "candle.Xm" 파싱 실패 시 대체값
        // market: 로그 문맥용
        std::optional<WsCandleResult> parseCandle(
            std::string_view json, int configured_fallback_unit,
            std::string_view market = "");
    };

} // namespace api::upbit
```

### 2. `src/api/upbit/WsMessageParser.cpp` (신규)

포함 헤더:
```cpp
#include "api/upbit/WsMessageParser.h"

#include <charconv>         // std::from_chars (parseMinuteCandleUnit에서 사용)
#include <json.hpp>
#include "api/upbit/dto/UpbitWsDtos.h"
#include "api/upbit/dto/UpbitQuotationDtos.h"
#include "api/upbit/mappers/MyOrderMapper.h"
#include "api/upbit/mappers/CandleMapper.h"
#include "util/Logger.h"
```

`parseMinuteCandleUnit()` 헬퍼를 anonymous namespace로 이전 (MarketEngineManager.cpp에서 그대로 복사):
```cpp
namespace {
    std::optional<int> parseMinuteCandleUnit(std::string_view type) noexcept {
        constexpr std::string_view prefix = "candle.";
        if (!type.starts_with(prefix) || type.size() <= prefix.size() + 1)
            return std::nullopt;
        if (type.back() != 'm')
            return std::nullopt;
        const std::string_view number = type.substr(prefix.size(), type.size() - prefix.size() - 1);
        int unit = 0;
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), unit);
        if (ec != std::errc{} || ptr != number.data() + number.size() || unit <= 0)
            return std::nullopt;
        return unit;
    }
}
```

`parseMyOrder()` 구현:
- JSON 파싱 실패 → `logger.error("[WsParser][", market, "] myOrder JSON parse failed")` + empty
- DTO 변환 실패 → `logger.error("[WsParser][", market, "] myOrder dto convert failed: ", e.what())` + empty
- `api::upbit::mappers::toEvents(dto)` → `vector<WsOrderEvent>` 반환

`parseCandle()` 구현:
- JSON 파싱 실패 → `logger.error("[WsParser][", market, "] candle JSON parse failed")` + nullopt
- `type.rfind("candle", 0) != 0` → silent nullopt (non-candle 정상 경로)
- `parseMinuteCandleUnit(type)` 실패 → `logger.warn("[WsParser][", market, "] candle unit parse failed, fallback=", fallback)` + fallback 사용
- DTO 변환 실패 → `logger.error("[WsParser][", market, "] candle dto convert failed: ", e.what())` + nullopt
- `api::upbit::mappers::toDomain(dto)` → `WsCandleResult` 반환

### 3. `src/api/CMakeLists.txt` 수정

```cmake
add_library(coinbot_api STATIC
    rest/RestClient.cpp
    rest/RestError.cpp
    auth/UpbitJwtSigner.cpp
    upbit/UpbitPublicRestClient.cpp
    upbit/UpbitExchangeRestClient.cpp
    upbit/SharedOrderApi.cpp
    upbit/WsMessageParser.cpp        # ← 추가
    ws/UpbitWebSocketClient.cpp
)
```

### 4. `src/app/MarketEngineManager.h` 수정

```cpp
// private 멤버 추가
#include "api/upbit/WsMessageParser.h"
// ...
private:
    api::upbit::WsMessageParser ws_parser_;
```

### 5. `src/app/MarketEngineManager.cpp` 수정

**제거할 include:**
```cpp
// 제거
#include <json.hpp>
#include "api/upbit/mappers/MyOrderMapper.h"
#include "api/upbit/mappers/CandleMapper.h"
```

**anonymous namespace에서 `parseMinuteCandleUnit()` 제거** (WsMessageParser.cpp로 이전, `<charconv>` include도 함께 제거 가능)

**`handleMyOrder_()` 단계 0~2 교체 (기존 ~20줄 → 2줄):**
```cpp
const auto events = ws_parser_.parseMyOrder(raw.json, ctx.market);
if (events.empty()) return;
```
이후 `has_trade` 감지 → 엔진 반영 루프(단계 3~4) 는 그대로 유지.

**`handleMarketData_()` 단계 0~2 교체 (기존 ~35줄 → 5줄):**
```cpp
const int configured_unit = util::AppConfig::instance().bot.live_candle_unit_minutes;
const auto result = ws_parser_.parseCandle(raw.json, configured_unit, ctx.market);
if (!result.has_value()) return;
const core::Candle& incoming = result->candle;
const int live_unit = result->unit_minutes;
```
이후 `doIntrabarCheck` / DB insert / 전략 실행(단계 3~5) 는 그대로 유지.

---

## 범위 외 (이번 작업 제외)

- **2단계**: `EngineInput`을 raw JSON에서 파싱된 도메인 타입으로 교체 → EventRouter에서 파싱 수행, IO 스레드 부하 검토 필요
- `WsMessageParser` 인터페이스화(테스트 주입용) — 현재 단일 전략이므로 불필요

---

## 검증 방법

### 빌드 / 정적 확인

1. `cmake --preset x64-debug && ninja` 오류 없음
2. `MarketEngineManager.cpp`에서 `json.hpp`, `MyOrderMapper.h`, `CandleMapper.h`, `UpbitWsDtos.h` 검색 → 0건

### 동작 케이스별 확인 (로그 또는 단위 테스트)

| 케이스 | 입력 | 기대 결과 |
|--------|------|-----------|
| **myOrder trade-first** | state="trade" + trade_uuid 있음 | `[TradeEvent]` 로그 먼저, `[OrderEvent]` 뒤 |
| **myOrder done-only** | state="done", trade_uuid 없음 | `[OrderEvent] done-only detected` 로그 + reconcileFromSnapshot 호출 |
| **myOrder malformed JSON** | `{bad json` | `[WsParser][KRW-XXX] myOrder JSON parse failed` 로그, 엔진 호출 없음 |
| **candle 정상** | type="candle.15m" | `[Candle] ts=... unit=15` 로그 |
| **candle non-type** | type="trade" | 로그 없음, silent drop |
| **candle unit fallback** | type="candle.??m" | `[WsParser][...] candle unit parse failed, fallback=15` 로그 |
| **candle malformed JSON** | `{bad json` | `[WsParser][...] candle JSON parse failed` 로그 |
