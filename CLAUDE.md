# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **문서 구조**:
> - **현재 아키텍처**: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
> - **미래 로드맵**: [docs/ROADMAP.md](docs/ROADMAP.md)
> - **구현 현황**: [docs/IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md)

## Build Commands

```bash
# Configure (from project root, using Visual Studio Developer Command Prompt)
cmake --preset x64-debug    # Debug build
cmake --preset x64-release  # Release build

# Build
cmake --build out/build/x64-debug
cmake --build out/build/x64-release

# Run
./out/build/x64-debug/CoinBot.exe
```

Requires: Visual Studio 2022 with MSVC, Ninja generator

## Dependencies (Hardcoded Paths)

- **Boost 1.89.0**: `C:/git-repository/boost_1_89_0` (thread, chrono components, static libs)
- **OpenSSL Win64**: `C:/git-repository/OpenSSL-Win64`
- **nlohmann JSON**: `C:/git-repository/nlohmann_json`

## Architecture Overview

CoinBot is a C++20 real-time cryptocurrency trading bot for the Upbit exchange.

### Core Data Flow

```
UpbitWebSocketClient (candles/tickers)
    ↓
MarketDataEventBridge
    ↓
EngineRunner (main event loop)
    ↓
RsiMeanReversionStrategy → Decision (buy/sell/hold)
    ↓
RealOrderEngine → UpbitExchangeRestClient (order submission)
    ↓
WebSocket (myOrder stream) → order fill/status updates
    ↓
Strategy state machine updates
```

### Key Modules

| Directory | Purpose |
|-----------|---------|
| `src/app/` | Entry point (`CoinBot.cpp`), `EngineRunner`, event bridges |
| `src/core/domain/` | Domain models: `Order`, `Candle`, `Account`, `Ticker`, `Orderbook` |
| `src/api/rest/` | Generic HTTPS client with retry logic |
| `src/api/ws/` | Boost.Beast WebSocket client with TLS, auto-reconnect |
| `src/api/upbit/` | Upbit-specific REST clients and DTOs |
| `src/api/upbit/mappers/` | DTO → Domain model converters |
| `src/api/auth/` | JWT token generation (`UpbitJwtSigner`) |
| `src/engine/` | Order execution: `RealOrderEngine`, `OrderStore` |
| `src/trading/strategies/` | Trading strategies (currently `RsiMeanReversionStrategy`) |
| `src/trading/indicators/` | Technical indicators: RSI, SMA, volatility |

### Strategy State Machine (RsiMeanReversionStrategy)

```
Flat → PendingEntry → InPosition → PendingExit → Flat
```

- **Flat**: No position, waiting for entry signal
- **PendingEntry**: Buy order placed, awaiting fill
- **InPosition**: Holding position with stop/target prices
- **PendingExit**: Sell order placed, awaiting fill

### Threading Model

- Main thread for startup
- Boost ASIO `io_context` thread for network I/O
- Separate WebSocket read loop thread
- `BlockingQueue` for inter-thread event passing

### Patterns Used

- **DTO + Mapper**: API responses → DTOs → Domain models
- **Event-driven**: Market data and order events flow through bridges to engine
- **Strategy pattern**: `IOrderEngine` interface, strategy decision output

# 파일 생성 규칙
- 모든 텍스트 파일은 한글이 깨지지 않도록 저장
- 오버코딩 금지
- 주석으로 왜 필요한지, 기능과 동작을 간단히 설명할 것
- 우선 테스트 코드는 작성, 수정하지 말고 요청 시 작성