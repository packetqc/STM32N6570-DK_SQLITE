# Live Session Capture — Portable Tooling

Stream-to-git bridge for real-time visual analysis by Claude Code. Works with any project.

---

## What This Provides

| File | Purpose |
|------|---------|
| `stream_capture.py` | Capture engine — WSL/Linux, 3 source modes, rolling MP4 clips, git sync |
| `dynamic/` | Output queue — rolling clips land here, git-tracked |
| `static/` | Input folder — drop pre-recorded videos for post-session analysis |

---

## How It Gets Into a Project

When Claude reads `packetqc/knowledge` on `wakeup`, the `live/` folder is part of the knowledge assets. If the active project doesn't have `live/`, Claude syncs it:

```
knowledge/live/             →  <project>/live/
  stream_capture.py                stream_capture.py
  dynamic/.gitkeep                 dynamic/.gitkeep
  static/.gitkeep                  static/.gitkeep
  README.md                        README.md
```

No clips are synced — only tooling. Clips are generated locally at runtime.

---

## Prerequisites

- **OBS Studio** (Windows) with [OBS RTSP Server plugin](https://github.com/iamscottxu/obs-rtsp)
- **WSL2** (Ubuntu) or native Linux
- **Python 3.7+**
- **ffmpeg**

---

## One-Time Setup

```bash
python3 live/stream_capture.py --setup
```

Installs ffmpeg, OpenCV, Pillow, and optional mss (WSLg screen capture).

---

## Quick Start

### 1. Start OBS RTSP Server (Windows)

1. Open OBS Studio
2. Set your scene to capture the board / serial terminal
3. Go to **Tools > RTSP Server**
4. Keep defaults: host `localhost`, port **8554**, path `/live`
5. Click **Start**

### 2. Launch Capture (WSL)

```bash
cd /path/to/your/project
python3 live/stream_capture.py --dynamic --rtsp rtsp://localhost:8554/live --scale 0.75 --crf 22 --push-interval 5
```

### 3. Tell Claude

```
I'm live
```

Claude pulls the latest clip, extracts the last frame, and reports what it sees.

### 4. Stop

Press **Ctrl+C** in WSL. Clips persist for review; cleaned at next start.

---

## Capture Modes

| Mode | Flag | Use Case |
|------|------|----------|
| RTSP stream | `--rtsp URL` | OBS on Windows, IP camera |
| File watch | `--file PATH` | PowerShell screenshot loop via `/mnt/c/...` |
| Screen grab | `--screen` | WSLg X11 (captures WSL GUI windows only) |

---

## Recommended Presets

| Use Case | Settings | Est. bandwidth |
|----------|----------|----------------|
| **QA session (recommended)** | `--scale 0.75 --crf 22 --push-interval 5` | ~250 MB/hr |
| UART text (sharp) | `--scale 1.0 --crf 22 --clip-secs 3` | ~400 MB/hr |
| High quality debug | `--scale 1.0 --crf 18 --fps 30` | ~500 MB/hr |
| Save bandwidth | `--fps 10 --clip-secs 5 --crf 32` | ~80 MB/hr |

---

## Multi-Source Capture

Run separate instances for different feeds:

```bash
# Terminal 1: UI capture
python3 live/stream_capture.py --dynamic --rtsp rtsp://localhost:8554/live --scale 0.75 --crf 22

# Terminal 2: UART capture (second OBS scene)
python3 live/stream_capture.py --dynamic --rtsp rtsp://localhost:8554/uart --scale 1.0 --crf 22 --prefix uart
```

Produces:
```
live/dynamic/
  clip_0.mp4, clip_1.mp4, clip_2.mp4     # UI feed
  uart_0.mp4, uart_1.mp4, uart_2.mp4     # UART feed
```

Use `multi-live` in Claude to get a comparative report across all sources.

---

## Claude Commands (from knowledge)

| Command | What Claude Does |
|---------|-----------------|
| `I'm live` | Pull latest clip, extract last frame, report UI/UART state |
| `multi-live` | Monitor all clip families, report comparative state |
| `deep <desc>` | Frame-by-frame forensic analysis of anomaly |
| `analyze <path>` | Static video analysis with state timeline |
| `recipe` | Print capture quick recipe with presets |

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `Cannot open RTSP` | Is OBS RTSP Server started? Check Tools > RTSP Server |
| No frames captured | Is OBS scene active (not blank)? |
| Permission denied | Run from `/mnt/d/...`, not a Windows-only path |
| Script not found | `cd` to repo root first |
| Git push rejected | Normal during live session — script retries automatically |
| Residual clips after Ctrl+C | Expected — cleaned at next start |

---

## Design Notes

- **Self-discovering paths**: Script uses `__file__` to find `dynamic/` and `static/` relative to itself
- **No project-specific hardcoding**: Same script works in any repo
- **Git integration**: Auto-detects branch, uses `git.exe` in WSL2 for Windows credentials
- **Squash on exit**: Rolls up clip commits into one (disable with `--no-squash`)
- **3-clip rolling queue**: `clip_0` > `clip_1` > `clip_2` > `clip_0`... (newest = highest)
