# SQLite High-Throughput Logging on STM32N6570-DK

> **2,800 logs/sec sustained** into SQLite on an SD card, from bare-metal Cortex-M55 @ 800 MHz — zero data loss, bounded memory, real backpressure.

This is a proof-of-concept for high-throughput structured logging on STM32 using SQLite WAL mode, PSRAM-backed double buffering, and ThreadX RTOS. The entire pipeline — from log generation through SQLite insertion to SD card persistence — runs on a single STM32N6570-DK board.

## Key Results

| Metric | Value |
|--------|-------|
| **Sustained ingestion rate** | ~2,600–2,800 logs/sec |
| **Peak rate (cold cache)** | 3,272 logs/sec |
| **Log struct size** | 224 bytes (cache-aligned) |
| **Effective throughput** | ~600 KB/s structured data into SQLite |
| **Data loss** | 0 (backpressure-guaranteed) |
| **SQLite heap usage** | ~15–27 KB / 1 MB (< 3%) |
| **PSRAM used** | ~12.2 MB / 32 MB available |

## Architecture

```
Simulator Thread ──captureLog()──> PSRAM Double Buffers (A/B, 16K logs each)
                                         │
                              READY flags │ FREE flags (backpressure)
                                         ▼
                                 Ingestor Thread
                              BEGIN ─> 16384 inserts ─> COMMIT
                                         │
                                    SQLite WAL
                              (1 MB heap + 4 MB pcache in PSRAM)
                                         │
                                      SD Card
                                     (logs.db)
```

See **[doc/readme.md](doc/readme.md)** for the full architecture documentation with Mermaid diagrams, memory layout, PRAGMA configuration, runtime data, and degradation analysis.

## Hardware

| Component | Spec |
|-----------|------|
| **MCU** | STM32N657X0H3QU — Cortex-M55 @ 800 MHz |
| **PSRAM** | APS256XX — 32 MB XSPI @ 200 MHz |
| **SD Card** | SDMMC2 — 4-bit @ 50 MHz (Class 10 U1) |
| **RTOS** | ThreadX (Azure RTOS) |
| **Filesystem** | FileX |

## How It Works

1. **Simulator thread** (P15) generates 224-byte `DS_LOG_STRUCT` records via `captureLog()`
2. **PSRAM double buffers** (2 x 16,384 logs = 7.2 MB) absorb bursts while the ingestor drains
3. **Backpressure** via ThreadX event flags — simulator blocks when both buffers are full, zero data loss guaranteed
4. **Ingestor thread** (P5) runs single-transaction batches of 16,384 inserts with prepared statements (`SQLITE_STATIC`)
5. **SQLite WAL mode** with `synchronous=OFF`, exclusive locking, 4 MB PSRAM page cache keeps B-tree interior pages hot
6. **Passive WAL checkpoints** every 5 buffers move WAL data into the main DB file on SD

## Project Structure

```
MPLIB-CODE/
  MPLIB_STORAGE.cpp    # Core pipeline: simulator, ingestor, captureLog, SQLite config
  MPLIB_STORAGE.h      # DS_LOG_STRUCT definition, class interface
SQLite/
  sqlite3.c/h          # SQLite amalgamation (unmodified)
doc/
  readme.md            # Full architecture docs + runtime data
  architecture.mmd     # Mermaid diagram source
Appli/                 # STM32CubeIDE application project
FSBL/                  # First Stage Boot Loader
Drivers/               # STM32 HAL/BSP drivers
```

## Building

This project uses **STM32CubeIDE** (or IAR EWARM). See the [Flashing Procedure](#flashing-procedure) below.

1. Open `STM32N6570-DK.ioc` in STM32CubeMX to review/regenerate configuration
2. Build both FSBL and Application from the IDE
3. Flash using the scripts in `Flash Scripts/`

### Flashing Procedure

1. Set BOOT1 switch to rightmost position, reset the board (programming mode)
2. Run the appropriate flash script from `Flash Scripts/`
3. After flashing, move BOOT1 to leftmost position and reset

## UART Output

Connect at **115200 baud** to see real-time pipeline stats:

```
--- STATS BLOCK ---------------------------------------------------------------------------
[STATS] SIMULATOR :  2958 logs/sec | Total:  376833
[STATS] INGESTION :  6553 logs/sec | Total:  360448 (Skipped: 0)
[STATS] PSRAM     : 16385 logs pending write
[STATS] SQLite Mem: 27148 / 1048576 bytes
--- STATS BLOCK ---------------------------------------------------------------------------
>> [INGEST] Buffer A Done | 6182 ms | Rate: 2650 l/s
>> [INGEST] Buffer B Done | 6498 ms | Rate: 2521 l/s
```

## License

[MIT](LICENSE)

## Acknowledgments

- **STM32N6570-DK** platform and HAL/BSP drivers by [STMicroelectronics](https://www.st.com) (licensed separately)
- **SQLite** — public domain database engine by [D. Richard Hipp](https://sqlite.org)
- **ThreadX / FileX** — Azure RTOS middleware by Microsoft (licensed separately)
- Development assisted by [Claude Code](https://claude.ai/code) (Anthropic)
