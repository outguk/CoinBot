# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Instructions

**Windows (Visual Studio + Ninja):** Build must be done through the Visual Studio Developer environment — do not attempt `cmake` or `ninja` from a regular cmd/bash shell as the toolchain is not in PATH.

```
# Configure (x64 Debug)
cmake --preset x64-debug

# Build
cd out/build/x64-debug && ninja
```

**Linux:**
```bash
cmake --preset linux-release
cd out/build/linux-release && make -j$(nproc)
```

**Dependencies (Windows):** Paths set via CMake cache variables in `CMakePresets.json` (`windows-base`):
- `COINBOT_BOOST_ROOT` → Boost 1.80+
- `COINBOT_OPENSSL_ROOT` → OpenSSL
- `COINBOT_NLOHMANN_DIR` → nlohmann/json

**Dependencies (Linux):** `apt install libboost-all-dev libssl-dev nlohmann-json3-dev`

## Running Locally

```bash
# Copy and fill in API keys
cp .env.local.example .env.local
bash scripts/run_local.sh
```

Environment variables required:
- `UPBIT_ACCESS_KEY`, `UPBIT_SECRET_KEY` — Upbit API keys
- `UPBIT_MARKETS` — optional CSV override, e.g. `KRW-BTC,KRW-ETH`

## Architecture Overview

CoinBot is a multi-market cryptocurrency trading bot for Upbit exchange. All layers use C++20.

### Layer Stack (bottom → top)

| Layer | Path | Responsibility |
|-------|------|----------------|
| **core** | `src/core/` | Domain types (`Order`, `Account`, `Candle`, etc.), `BlockingQueue` |
| **util** | `src/util/` | `Logger`, `AppConfig` (singleton), `UrlCodec` |
| **database** | `src/database/` | Vendored SQLite + `Database` wrapper |
| **api** | `src/api/` | REST client, WebSocket client, Upbit DTOs + mappers, JWT signer |
| **trading** | `src/trading/` | Indicators (RSI, SMA, volatility), `RsiMeanReversionStrategy`, `AccountManager` |
| **engine** | `src/engine/` | `MarketEngine` (per-market order lifecycle), `OrderStore` |
| **app** | `src/app/` | `MarketEngineManager`, `EventRouter`, `StartupRecovery` |

### Concurrency Model

- **`MarketEngineManager`** owns one `std::jthread` per market; each thread pops from a `BlockingQueue<EngineInput>`.
- **`UpbitWebSocketClient`** runs its own `std::jthread`; raw JSON arrives on the IO thread and is forwarded via `EventRouter` → `BlockingQueue` into market worker queues.
- **`AccountManager`** is shared across market workers and uses an internal mutex.
- Worker abnormal exit is detected via `AbnormalExitGuard` RAII guard; `MarketEngineManager::hasFatalWorker()` is polled in the main loop → `std::exit(1)` → systemd `Restart=on-failure`.

### Key Design Decisions

**Fund reservation model:** `AccountManager` uses a reservation token pattern. A `ReservationToken` locks KRW at buy-signal time; it is released (→ available_krw) on cancel, or converted to coin balance on fill.

**All-in invariant:** `(coin_balance > 0) XOR (available_krw > 0)` per market. A market is either fully in KRW or fully in coin.

**EngineInput carries raw JSON:** `EventRouter` wraps messages as `MyOrderRaw{json}` / `MarketDataRaw{json}`. Parsing (JSON → DTO → domain) happens inside `MarketEngineManager::handleMyOrder_()` / `handleMarketData_()`, not in the API layer.

**Intrabar exits:** `RsiMeanReversionStrategy::onIntrabarCandle()` checks stop/target prices on every live (unconfirmed) candle tick; RSI-based exits only fire on confirmed candle close.

### Configuration

All runtime configuration lives in `src/util/Config.h` as inline defaults in `AppConfig`. No config file is loaded at runtime — defaults are compile-time. Override markets via `UPBIT_MARKETS` env var.

Key thresholds:
- `coin_epsilon = 1e-8` — quantity-level dust cutoff
- `init_dust_threshold_krw = 5000.0` — value-level dust cutoff (matches exchange minimum order)
- `min_notional_krw = 5000.0` — strategy minimum order size

### Logging

`util::Logger::instance()` is the sole logging path in production code. `std::cout` must not be used in `src/`. Level-based flush: `WARN`/`ERROR` → `std::flush`, `INFO`/`DEBUG` → buffered.

Log level mapping convention:
- `error()` — unrecoverable failures, rejected orders, WS/REST hard errors
- `warn()` — recoverable issues, retries, soft failures
- `info()` — state transitions, startup/shutdown events
- `debug()` — high-frequency detail (per-message RX, order ENTER/REQ)

### Database

`db::Database` wraps SQLite (vendored `src/database/sqlite3.c`). Schema is applied on `open()`. DB file defaults to `db/coinbot.db` (relative to CWD). On Windows, `CoinBot.cpp::main()` auto-searches upward from the executable for the `db/` directory.

`sqlite3.c`/`sqlite3.h` are local-only (in `.gitignore`).

## Coding Guidelines (from AGENTS.md) (always apply)

- Comments should explain **why**, not just what.
- Do not modify test code unless explicitly asked.
- Do not over-engineer; preserve working code with minimal changes.
- Readability over cleverness.
- Don't try to build in a VS environment because it is not possible to build in cmd
- When explaining logic, don't lie if there's something wrong, but if there's something wrong, explain the wrong part
