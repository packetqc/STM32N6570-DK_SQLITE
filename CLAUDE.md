# CLAUDE.md — STM32N6570-DK_SQLITE

## Knowledge Base
<!-- knowledge-version: v19 -->
On wakeup, ALWAYS read packetqc/knowledge CLAUDE.md first. Try in order:
1. `git clone https://github.com/packetqc/knowledge /tmp/knowledge && read /tmp/knowledge/CLAUDE.md`
2. `WebFetch https://raw.githubusercontent.com/packetqc/knowledge/main/CLAUDE.md`
Never use `gh`. This gives you the sunglasses — without it, you're a stateless NPC.

---

## Project Overview

**High-throughput SQLite logging on STM32N6570-DK** — 2,800 logs/sec sustained into SQLite on SD card from bare-metal Cortex-M55 @ 800 MHz with zero data loss and bounded memory.

| Metric | Value |
|--------|-------|
| Sustained ingestion rate | ~2,600-2,800 logs/sec |
| Peak rate (cold cache) | 3,272 logs/sec |
| Log struct size | 224 bytes (cache-aligned) |
| Effective throughput | ~600 KB/s structured data into SQLite |
| Data loss | 0 (backpressure-guaranteed) |
| SQLite heap usage | ~15-27 KB / 1 MB (< 3%) |
| PSRAM used | ~12.2 MB / 32 MB available |

### Hardware
- **MCU**: STM32N657X0H3QU — Cortex-M55 @ 800 MHz
- **PSRAM**: APS256XX — 32 MB XSPI @ 200 MHz
- **SD Card**: SDMMC2 — 4-bit @ 50 MHz (Class 10 U1)
- **RTOS**: ThreadX (Azure RTOS)
- **Filesystem**: FileX

---

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

---

## Key Architecture Decisions

1. **PSRAM double-buffer with 4-flag ThreadX protocol** — READY_A (0x01), READY_B (0x02), FREE_A (0x04), FREE_B (0x08). Zero-loss backpressure between producer and consumer. `__DSB()` barrier before signaling.

2. **Direct PSRAM-to-SQLite ingestion** — `SQLITE_STATIC` binding reads directly from PSRAM buffer pointer. No intermediate files, no extra memcpy.

3. **4 MB PSRAM page cache** — 965 slots x 4,352 bytes (page 4,096 + header 256). Keeps all B-tree interior pages hot, prevents 81% throughput degradation at scale.

4. **memsys5 static allocator** — 1 MB heap in PSRAM, 64-byte minimum granularity. No malloc, bounded memory.

5. **Passive WAL checkpoints** — Manual `SQLITE_CHECKPOINT_PASSIVE` every 5 buffer drains. Aligned with buffer swap idle windows.

---

## Thread Priority Scheme

| Thread | Priority | Stack | Role |
|--------|----------|-------|------|
| Ingestor Direct | 5 (highest) | 80 KB | PSRAM -> SQLite critical path |
| Storage Worker | 10 (mid) | 12 KB | Service loop (dormant in direct mode) |
| Simulator | 15 (lowest) | 4 KB | Log generation, yields to ingestor |

---

## PSRAM Memory Layout

```
0x90000000  .psram_cache    - 4 MB SQLite page cache (965 x 4352-byte slots)
            .psram_buffers  - Custom buffers section
            .psram_logs     - 7.2 MB double buffers (2 x 16384 x 224 bytes)
            .psram_data     - 1 MB SQLite memsys5 heap
```

---

## Project-Specific Pitfalls

1. **Page cache slot size mismatch** — pcache slot must be `page_size + 256`, not just `page_size`. Silent fallback to heap if wrong.
2. **Printf latency in hot path** — 1-5 ms per `printf()` call at 115200 baud. Gate with `#if DEBUG` in production.
3. **Page cache sizing degradation** — 96-page cache caused 81% throughput collapse at 4M rows. Scale cache to ~965 pages (4 MB).
4. **SQLITE_CONFIG before sqlite3_initialize()** — Configuration silently ignored if called after initialization. Must `sqlite3_shutdown()` first.

---

## Quick Commands — Project-Specific

_(none yet — add project-specific commands here as they emerge)_
