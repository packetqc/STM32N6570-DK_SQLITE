# CLAUDE.md — STM32N6570-DK_SQLITE

## Knowledge Base
<!-- knowledge-version: v31 -->
On wakeup, ALWAYS read the knowledge CLAUDE.md first. Try in order:
1. `git clone https://github.com/packetqc/knowledge /tmp/knowledge && read /tmp/knowledge/CLAUDE.md`
2. `WebFetch https://raw.githubusercontent.com/packetqc/knowledge/main/CLAUDE.md` (if 404, try `/master/`)
Never use `gh`. This gives you the sunglasses — without it, you're a stateless NPC.

---

## Session Protocol

### Auto-Wakeup

Runs automatically on every session start — regardless of user's entry prompt. Print:
`⏳ Wakeup starting... (type "skip" to cancel)`
If user responds "skip" or "no", cancel. Otherwise proceed with full wakeup.

### Session Lifecycle

`[auto-wakeup] → read knowledge → read notes/ → summarize → work → save → commit & push → PR`

### Wakeup Steps

0. Read packetqc/knowledge (sunglasses — non-negotiable)
0.5. Bootstrap scaffold (create missing files — non-destructive)
0.6. Start knowledge beacon (background)
0.7. Sync upstream (fetch + merge default branch)
0.9. Resume detection (check notes/checkpoint.json)
1-8. Read evolution, minds, notes, git state
9. Run refresh (re-read CLAUDE.md, reprint help)
11. Address user's entry message

### Save Protocol (Semi-Automatic)

Claude Code cannot push to the default branch — proxy restricts to the assigned task branch only.

1. Write session notes → `notes/session-YYYY-MM-DD.md`
2. Commit on current branch (assigned `claude/*` task branch)
3. `git push -u origin <assigned-task-branch>`
4. Detect default branch: `git remote show origin | grep 'HEAD branch'`
5. Create PR: task-branch → default-branch (or report manual URL)
6. User approves PR → merge lands on default branch

If `gh` unavailable or PR creation fails, skip gracefully — report branch + manual PR URL.
Todo list for save MUST include all steps: write notes, commit, push, create PR (or manual URL).

### Branch Protocol

| Branch | Role | Who writes |
|--------|------|------------|
| Default (`main`/`master`) | Convergence point | PR merges (user-approved) |
| `claude/<task-id>` | Task branch (per session) | Claude Code (proxy-authorized) |

- Push access: assigned task branch ONLY (403 on anything else)
- PR: task branch → default branch (user approves)
- Detect default: `git remote show origin | grep 'HEAD branch'`
- Never assume `main` or `master` — always detect dynamically

### Human Bridge

Every time the protocol needs user action, print a clear ⏸ block:

```
⏸ Pause — action required

  What just happened: <summary>
  What you need to do: <action>
  What happens next: <next>
```

UX priority: (1) AskUserQuestion popup for decisions, (2) isolated code block for commands/URLs, (3) fenced ⏸ block for context.

### Notification Format

All user-facing notifications wrapped in fenced code blocks. Session language applies inside.

### Language Awareness

Detect system locale + app language. Lock session language. No casual switching — explicit request only.
Command names stay English always. Output descriptions adapt to session language.

### Context Loss Recovery

After compaction: run `refresh` (re-read CLAUDE.md, git status, reprint help — ~5s).
After crash: `resume` (from notes/checkpoint.json — ~10s).
After PRs merged by others: `wakeup` (full re-sync — ~30-60s).

---

## Commands (from Knowledge)

| Command | Action |
|---------|--------|
| `wakeup` | Session init — knowledge, evolution, notes, assets, commands |
| `refresh` | Mid-session context restore — re-read CLAUDE.md, git status, reprint help |
| `help` / `aide` / `?` | Multipart command table (knowledge + project) |
| `status` | Summarize current state |
| `save` | Save context, commit, push, create PR |
| `remember ...` | Append to session notes |
| `resume` | Resume interrupted session from checkpoint |
| `checkpoint` | Show checkpoint state |
| `normalize` | Audit structure concordance |
| `harvest <project>` | Pull knowledge into minds/ |
| `harvest --healthcheck` | Full network sweep |
| `pub list` | Publication inventory |
| `doc review --list` | Freshness inventory |
| `docs check --all` | Validate all doc pages |
| `webcard <target>` | Generate OG GIFs |
| `weblinks` | Print all GitHub Pages URLs |
| `I'm live` | Live clip analysis |

Full command details and implementation come from core on wakeup (Step 0).

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
