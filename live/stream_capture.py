#!/usr/bin/env python3
"""
MPLIB Live Session Capture — Stream-to-Git bridge (WSL-native)
==============================================================
Captures rolling MP4 clips and drops them into a git-synced queue
folder for Claude to grab near-live.

Designed to run from WSL in the repo folder. Three capture strategies:

  1. --screen     WSLg X11 screen capture (captures WSL GUI windows)
  2. --file PATH  Watch a file (use /mnt/c/... for Windows-side files)
  3. --rtsp URL   RTSP stream (OBS on Windows, IP camera, etc.)

For capturing the WINDOWS DESKTOP from WSL, use one of:
  a) OBS → RTSP plugin → --rtsp rtsp://localhost:8554/live
  b) PowerShell screenshot loop → --file /mnt/c/Users/YOU/frame.png
     (see --win-helper to generate the PowerShell script)

QUICK START (WSL):
    # 1. Install deps (once)
    python3 live/stream_capture.py --setup

    # 2. Run (pick one)
    python3 live/stream_capture.py --dynamic --screen                     # WSLg
    python3 live/stream_capture.py --dynamic --file /mnt/c/Users/X/f.png  # Windows file
    python3 live/stream_capture.py --dynamic --rtsp rtsp://HOST:8554/live  # RTSP

    # 3. Tell Claude: "Check live/dynamic/ — I'm streaming"

OPTIONS:
    --fps 30            Capture framerate (default: 30)
    --clip-secs 2       Seconds per clip (default: 2)
    --push-interval 3   Git push interval (default: 3s)
    --region x,y,w,h    Crop region
    --monitor 1         Monitor number (screen mode)
    --scale 0.5         Downscale (default: 0.5)
    --crf 28            MP4 quality 18=sharp 28=compact (default: 28)
    --keep-clips 3      Rolling queue size (default: 3)
    --no-push           Capture only, skip git push (manual sync)
    --no-squash         Keep all commits on exit
"""

import argparse
import subprocess
import sys
import time
import os
import signal
import threading
import shutil

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT   = os.path.dirname(SCRIPT_DIR)
STATIC_DIR  = os.path.join(SCRIPT_DIR, "static")
DYNAMIC_DIR = os.path.join(SCRIPT_DIR, "dynamic")

os.makedirs(STATIC_DIR, exist_ok=True)
os.makedirs(DYNAMIC_DIR, exist_ok=True)

# ---------------------------------------------------------------------------
# WSL detection
# ---------------------------------------------------------------------------
def is_wsl():
    try:
        with open("/proc/version", "r") as f:
            return "microsoft" in f.read().lower()
    except Exception:
        return False

def is_wslg():
    """Check if WSLg (GUI support) is available."""
    return os.path.exists("/mnt/wslg") or os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY")

def get_windows_host_ip():
    """Get the Windows host IP from WSL2 (via /etc/resolv.conf nameserver)."""
    try:
        with open("/etc/resolv.conf", "r") as f:
            for line in f:
                if line.strip().startswith("nameserver"):
                    ip = line.strip().split()[1]
                    if ip != "127.0.0.1":
                        return ip
    except Exception:
        pass
    return None

def fix_wsl_url(url):
    """Replace localhost/127.0.0.1 with Windows host IP when running in WSL2."""
    if not is_wsl():
        return url
    if "localhost" not in url and "127.0.0.1" not in url:
        return url
    host_ip = get_windows_host_ip()
    if not host_ip:
        return url
    fixed = url.replace("localhost", host_ip).replace("127.0.0.1", host_ip)
    print(f"[LIVE] WSL2 detected — remapping to Windows host: {fixed}")
    return fixed


# ---------------------------------------------------------------------------
# Setup — WSL-native with apt + pip
# ---------------------------------------------------------------------------
def run_setup():
    wsl = is_wsl()
    wslg = is_wslg()

    print("[SETUP] === MPLIB Live Capture — Dependency Setup ===")
    print(f"[SETUP] Platform: {'WSL' if wsl else 'Linux'} | WSLg: {'yes' if wslg else 'no'}\n")

    # System packages (ffmpeg)
    if shutil.which("ffmpeg"):
        result = subprocess.run(["ffmpeg", "-version"], capture_output=True, text=True)
        ver = result.stdout.split("\n")[0] if result.stdout else "?"
        print(f"[SETUP] ffmpeg: OK ({ver})")
    else:
        print("[SETUP] Installing ffmpeg...")
        r = subprocess.run(["sudo", "apt", "install", "-y", "ffmpeg"],
                           capture_output=True, text=True)
        if r.returncode == 0:
            print("[SETUP] ffmpeg: OK")
        else:
            print(f"[SETUP] ffmpeg: FAILED — run manually: sudo apt install ffmpeg")

    # Python packages
    packages = ["Pillow"]
    if wslg:
        packages.append("mss")  # screen capture only works with WSLg
    packages.append("opencv-python-headless")  # headless for WSL (no GUI deps)

    for pkg in packages:
        print(f"[SETUP] pip: {pkg}...", end=" ")
        r = subprocess.run(
            [sys.executable, "-m", "pip", "install", "--quiet", "--break-system-packages", pkg],
            capture_output=True, text=True
        )
        if r.returncode == 0:
            print("OK")
        else:
            # Try without --break-system-packages (older pip)
            r2 = subprocess.run(
                [sys.executable, "-m", "pip", "install", "--quiet", pkg],
                capture_output=True, text=True
            )
            print("OK" if r2.returncode == 0 else f"WARN: {r2.stderr.strip()[:80]}")

    print()
    if wslg:
        print("[SETUP] WSLg detected — --screen mode will work (captures WSL GUI)")
    else:
        print("[SETUP] No WSLg — use --file /mnt/c/... or --rtsp for Windows capture")

    if wsl:
        print("[SETUP]")
        print("[SETUP] To capture WINDOWS desktop from WSL:")
        print("[SETUP]   Option A: python3 live/stream_capture.py --win-helper")
        print("[SETUP]             (generates a PowerShell screenshot script)")
        print("[SETUP]   Option B: OBS + RTSP plugin → --rtsp rtsp://localhost:8554/live")

    print(f"\n[SETUP] Ready. Run: python3 live/stream_capture.py --dynamic --screen")


# ---------------------------------------------------------------------------
# Windows helper — PowerShell screenshot script for --file mode
# ---------------------------------------------------------------------------
def generate_win_helper():
    """Generate a PowerShell script that captures Windows screen to a file."""
    # Get Windows username from /mnt/c/Users/
    win_user = "YOUR_USER"
    try:
        users_dir = "/mnt/c/Users"
        if os.path.isdir(users_dir):
            candidates = [d for d in os.listdir(users_dir)
                         if d not in ("Public", "Default", "Default User", "All Users")
                         and os.path.isdir(os.path.join(users_dir, d))]
            if candidates:
                win_user = candidates[0]
    except Exception:
        pass

    win_path = f"C:\\Users\\{win_user}\\mplib_frame.png"
    wsl_path = f"/mnt/c/Users/{win_user}/mplib_frame.png"

    ps_script = f'''# MPLIB Live Capture — Windows Screen Grabber
# Save as: capture_screen.ps1
# Run in PowerShell: .\\capture_screen.ps1
# Then in WSL: python3 live/stream_capture.py --dynamic --file {wsl_path}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$output = "{win_path}"
$fps = 10  # 10 fps is enough for UART reading
$interval = [int](1000 / $fps)

Write-Host "Capturing screen to $output at $fps fps (Ctrl+C to stop)"

while ($true) {{
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap($screen.Width, $screen.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($screen.Location, [System.Drawing.Point]::Empty, $screen.Size)
    $bitmap.Save($output, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
    Start-Sleep -Milliseconds $interval
}}
'''

    helper_path = os.path.join(SCRIPT_DIR, "capture_screen.ps1")
    with open(helper_path, "w", newline="\r\n") as f:
        f.write(ps_script)

    print(f"[LIVE] Generated: live/capture_screen.ps1")
    print(f"[LIVE]")
    print(f"[LIVE] Step 1 — On Windows (PowerShell):")
    print(f"[LIVE]   cd to repo, then: .\\live\\capture_screen.ps1")
    print(f"[LIVE]")
    print(f"[LIVE] Step 2 — In WSL:")
    print(f"[LIVE]   python3 live/stream_capture.py --dynamic --file {wsl_path}")
    print(f"[LIVE]")
    print(f"[LIVE] This captures Windows screen → PNG file → WSL reads it → MP4 clips → git push")


# ---------------------------------------------------------------------------
# Graceful shutdown
# ---------------------------------------------------------------------------
running = True

def signal_handler(sig, frame):
    global running
    print("\n[LIVE] Ctrl+C — flushing last clip + final push...")
    running = False

signal.signal(signal.SIGINT, signal_handler)


# ---------------------------------------------------------------------------
# Git push thread
# ---------------------------------------------------------------------------
def _git_cmd():
    """Return 'git.exe' in WSL2 (uses Windows credentials), 'git' otherwise."""
    if is_wsl():
        # Prefer git.exe (Windows credentials) — check common paths if not in PATH
        if shutil.which("git.exe"):
            return "git.exe"
        common_paths = [
            "/mnt/c/Program Files/Git/cmd/git.exe",
            "/mnt/c/Program Files (x86)/Git/cmd/git.exe",
        ]
        for p in common_paths:
            if os.path.exists(p):
                return p
        print("  [GIT] WARN: git.exe not found — falling back to Linux git (may lack credentials)")
    return "git"

def _git_branch(git_cmd):
    """Detect current branch name."""
    try:
        result = subprocess.run(
            [git_cmd, "rev-parse", "--abbrev-ref", "HEAD"],
            cwd=REPO_ROOT, capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return None

class GitPusher(threading.Thread):
    def __init__(self, push_interval, dynamic_rel="live/dynamic"):
        super().__init__(daemon=True)
        self.push_interval = push_interval
        self.dynamic_rel = dynamic_rel
        self.push_count = 0
        self.lock = threading.Lock()
        self.git = _git_cmd()
        self.branch = _git_branch(self.git)
        print(f"  [GIT] Using:  {self.git}")
        print(f"  [GIT] Branch: {self.branch}")
        print(f"  [GIT] cwd:    {REPO_ROOT}")
        self._stop = threading.Event()

    def run(self):
        while not self._stop.is_set():
            self._stop.wait(self.push_interval)
            if self._stop.is_set():
                break
            self._do_push()

    def _do_push(self):
        with self.lock:
            try:
                subprocess.run(
                    [self.git, "add", self.dynamic_rel],
                    cwd=REPO_ROOT, capture_output=True, timeout=10
                )
                result = subprocess.run(
                    [self.git, "commit", "-m", "live: clip update"],
                    cwd=REPO_ROOT, capture_output=True, text=True, timeout=10
                )
                if result.returncode != 0:
                    if "nothing to commit" in (result.stdout + result.stderr):
                        return
                push_cmd = [self.git, "push"]
                if self.branch:
                    push_cmd = [self.git, "push", "-u", "origin", self.branch]
                for attempt in range(4):
                    push = subprocess.run(
                        push_cmd,
                        cwd=REPO_ROOT, capture_output=True, text=True, timeout=30
                    )
                    if push.returncode == 0:
                        self.push_count += 1
                        print(f"  [GIT] push #{self.push_count}")
                        return
                    if attempt == 0:
                        err = (push.stderr or push.stdout or "")[:120]
                        print(f"  [GIT] push error: {err}")
                    time.sleep(2 ** attempt)
                print("  [GIT] push failed after 4 retries")
            except Exception as e:
                print(f"  [GIT] error: {e}")

    def final_push(self):
        self._do_push()

    def stop(self):
        self._stop.set()

    def cleanup_commits(self):
        with self.lock:
            try:
                result = subprocess.run(
                    [self.git, "log", "--oneline", "-200", "--format=%s"],
                    cwd=REPO_ROOT, capture_output=True, text=True, timeout=10
                )
                if result.returncode != 0:
                    return
                lines = result.stdout.strip().split("\n")
                count = 0
                for line in lines:
                    if line.strip() in ("live: clip update", "live: frame update"):
                        count += 1
                    else:
                        break
                if count > 1:
                    print(f"[LIVE] Squashing {count} clip commits...")
                    subprocess.run([self.git, "reset", "--soft", f"HEAD~{count}"],
                                   cwd=REPO_ROOT, capture_output=True, timeout=10)
                    subprocess.run([self.git, "commit", "-m",
                                    f"live: session capture ({count} syncs)"],
                                   cwd=REPO_ROOT, capture_output=True, timeout=10)
                    subprocess.run([self.git, "push", "--force-with-lease"],
                                   cwd=REPO_ROOT, capture_output=True, timeout=30)
                    print(f"[LIVE] {count} commits → 1")
            except Exception as e:
                print(f"[LIVE] Cleanup warning: {e}")


# ---------------------------------------------------------------------------
# Frame grabbers — return PIL Image
# ---------------------------------------------------------------------------
def grab_screen(sct, monitor, region):
    from PIL import Image
    if region:
        shot = sct.grab(region)
    else:
        shot = sct.grab(sct.monitors[monitor])
    return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def grab_file(src_path):
    from PIL import Image
    return Image.open(src_path).convert("RGB")


def grab_rtsp(cap, region):
    from PIL import Image
    import cv2
    ret, frame = cap.read()
    if not ret:
        raise RuntimeError("RTSP read failed")
    if region:
        x, y, w, h = region["left"], region["top"], region["width"], region["height"]
        frame = frame[y:y+h, x:x+w]
    return Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))


# ---------------------------------------------------------------------------
# MP4 encoder — ffmpeg pipe
# ---------------------------------------------------------------------------
def encode_clip_mp4(frames, output_path, fps, scale, crf):
    from PIL import Image
    if not frames:
        return 0

    if scale != 1.0:
        w = int(frames[0].width * scale)
        h = int(frames[0].height * scale)
        w = w if w % 2 == 0 else w + 1
        h = h if h % 2 == 0 else h + 1
        frames = [f.resize((w, h), Image.LANCZOS) for f in frames]
    else:
        w = frames[0].width if frames[0].width % 2 == 0 else frames[0].width + 1
        h = frames[0].height if frames[0].height % 2 == 0 else frames[0].height + 1
        if w != frames[0].width or h != frames[0].height:
            frames = [f.resize((w, h), Image.LANCZOS) for f in frames]

    width, height = frames[0].size

    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "rawvideo",
        "-pix_fmt", "rgb24",
        "-s", f"{width}x{height}",
        "-r", str(fps),
        "-i", "pipe:0",
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-crf", str(crf),
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        "-an",
        output_path
    ]

    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )

    for frame in frames:
        proc.stdin.write(frame.tobytes())

    proc.stdin.close()
    proc.wait(timeout=30)

    if proc.returncode != 0:
        err = proc.stderr.read().decode()[:200] if proc.stderr else ""
        print(f"  [FFMPEG] encode error: {err}")
        return 0

    return os.path.getsize(output_path) if os.path.exists(output_path) else 0


# ---------------------------------------------------------------------------
# Clip queue — rotates clip_0.mp4 .. clip_N.mp4
# ---------------------------------------------------------------------------
class ClipQueue:
    def __init__(self, directory, keep=3):
        self.directory = directory
        self.keep = keep
        self.index = 0

    def next_path(self):
        name = f"clip_{self.index % self.keep}.mp4"
        self.index += 1
        return os.path.join(self.directory, name)

    def total_size_kb(self):
        total = 0
        for f in os.listdir(self.directory):
            if f.startswith("clip_") and f.endswith(".mp4"):
                total += os.path.getsize(os.path.join(self.directory, f))
        return total / 1024


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="MPLIB Live Session Capture — WSL-native MP4 rolling clips")
    parser.add_argument("--setup",         action="store_true",       help="Install deps (apt + pip)")
    parser.add_argument("--win-helper",    action="store_true",       help="Generate PowerShell capture script")
    parser.add_argument("--dynamic",       action="store_true",       help="Enable dynamic capture")
    parser.add_argument("--screen",        action="store_true",       help="Source: screen (WSLg)")
    parser.add_argument("--file",          type=str,   default=None,  help="Source: watch file (/mnt/c/...)")
    parser.add_argument("--rtsp",          type=str,   default=None,  help="Source: RTSP URL")
    parser.add_argument("--fps",           type=int,   default=30,    help="Framerate (default: 30)")
    parser.add_argument("--clip-secs",     type=float, default=2,     help="Secs/clip (default: 2)")
    parser.add_argument("--push-interval", type=float, default=3,     help="Git push interval (default: 3s)")
    parser.add_argument("--region",        type=str,   default=None,  help="Crop: x,y,w,h")
    parser.add_argument("--monitor",       type=int,   default=1,     help="Monitor (screen mode)")
    parser.add_argument("--scale",         type=float, default=0.5,   help="Downscale (default: 0.5)")
    parser.add_argument("--crf",           type=int,   default=28,    help="Quality 18-35 (default: 28)")
    parser.add_argument("--keep-clips",    type=int,   default=3,     help="Queue size (default: 3)")
    parser.add_argument("--no-push",       action="store_true",       help="Skip git push (manual sync)")
    parser.add_argument("--no-squash",     action="store_true",       help="Keep all commits")
    args = parser.parse_args()

    # --- SETUP ---
    if args.setup:
        run_setup()
        return

    # --- WIN HELPER ---
    if args.win_helper:
        generate_win_helper()
        return

    # --- STATIC MODE ---
    if not args.dynamic:
        wsl = is_wsl()
        print("[LIVE] === MPLIB Live Capture ===")
        print(f"[LIVE] Platform: {'WSL' if wsl else 'Linux'} | WSLg: {'yes' if is_wslg() else 'no'}")
        print("[LIVE]")
        print("[LIVE] Quick start:")
        print("[LIVE]   python3 live/stream_capture.py --setup      # install deps (once)")
        print("[LIVE]   python3 live/stream_capture.py --dynamic --screen  # capture WSLg")
        if wsl:
            print("[LIVE]")
            print("[LIVE] For Windows desktop capture:")
            print("[LIVE]   python3 live/stream_capture.py --win-helper    # generates PS1 script")
        print("[LIVE]")
        print("[LIVE] Static mode: drop files into live/static/, commit manually")
        return

    # --- DYNAMIC MODE ---
    if not (args.screen or args.file or args.rtsp):
        print("[LIVE] ERROR: --dynamic needs: --screen, --file PATH, or --rtsp URL")
        sys.exit(1)

    if not shutil.which("ffmpeg"):
        print("[LIVE] ERROR: ffmpeg not in PATH. Run: python3 live/stream_capture.py --setup")
        sys.exit(1)

    # Parse region
    region = None
    if args.region:
        parts = [int(x) for x in args.region.split(",")]
        if len(parts) == 4:
            region = {"left": parts[0], "top": parts[1],
                      "width": parts[2], "height": parts[3]}

    # Init source
    sct = None
    cap = None
    source_label = ""

    if args.screen:
        if not is_wslg():
            print("[LIVE] WARN: WSLg not detected — screen capture may not work")
            print("[LIVE] Consider: --file /mnt/c/... or --rtsp instead")
        try:
            import mss
            sct = mss.mss()
            source_label = f"screen/WSLg (monitor {args.monitor})"
        except ImportError:
            print("[LIVE] mss not installed. Run: python3 live/stream_capture.py --setup")
            sys.exit(1)
    elif args.file:
        if not os.path.exists(args.file):
            # Check if it's a /mnt/c path that doesn't exist yet
            if args.file.startswith("/mnt/c"):
                print(f"[LIVE] Waiting for file: {args.file}")
                print(f"[LIVE] Start your Windows capture tool, then this will begin...")
                while not os.path.exists(args.file) and running:
                    time.sleep(0.5)
                if not running:
                    return
                print(f"[LIVE] File appeared!")
            else:
                print(f"[LIVE] File not found: {args.file}")
                sys.exit(1)
        source_label = f"file ({args.file})"
    elif args.rtsp:
        try:
            import cv2
            rtsp_url = fix_wsl_url(args.rtsp)
            cap = cv2.VideoCapture(rtsp_url)
            if not cap.isOpened():
                print(f"[LIVE] Cannot open RTSP: {rtsp_url}")
                sys.exit(1)
            source_label = f"rtsp ({rtsp_url})"
        except ImportError:
            print("[LIVE] opencv not installed. Run: python3 live/stream_capture.py --setup")
            sys.exit(1)

    clip_frames = int(args.fps * args.clip_secs)
    frame_time = 1.0 / args.fps

    # Clean up residual clips from previous session
    cleaned = 0
    for f in os.listdir(DYNAMIC_DIR):
        if f.startswith("clip_") and f.endswith(".mp4"):
            os.remove(os.path.join(DYNAMIC_DIR, f))
            cleaned += 1
    if cleaned:
        print(f"[LIVE] Cleaned {cleaned} residual clips from previous session")

    print(f"[LIVE] === DYNAMIC MODE — Rolling MP4 Clips ===")
    print(f"[LIVE] Source:   {source_label}")
    print(f"[LIVE] Capture:  {args.fps} fps | {args.clip_secs}s/clip ({clip_frames} frames)")
    print(f"[LIVE] Encode:   H.264 ultrafast CRF {args.crf} | scale {args.scale}x")
    print(f"[LIVE] Queue:    {args.keep_clips} clips → live/dynamic/")
    if not args.no_push:
        print(f"[LIVE] Git:      push every {args.push_interval}s (async)")
    else:
        print(f"[LIVE] Git:      disabled (--no-push)")
    print(f"[LIVE] Ctrl+C to stop\n")

    # Start git push thread (unless --no-push)
    pusher = None
    if not args.no_push:
        pusher = GitPusher(args.push_interval)
        pusher.start()

    queue = ClipQueue(DYNAMIC_DIR, keep=args.keep_clips)
    frame_buffer = []
    total_frames = 0
    total_clips = 0
    last_mtime = 0

    while running:
        t0 = time.perf_counter()

        try:
            img = None
            if args.screen:
                img = grab_screen(sct, args.monitor, region)
            elif args.file:
                try:
                    mtime = os.path.getmtime(args.file)
                except OSError:
                    time.sleep(frame_time)
                    continue
                if mtime == last_mtime:
                    time.sleep(frame_time)
                    continue
                last_mtime = mtime
                img = grab_file(args.file)
            elif args.rtsp:
                img = grab_rtsp(cap, region)

            if img is None:
                time.sleep(frame_time)
                continue

            frame_buffer.append(img)
            total_frames += 1

            if len(frame_buffer) >= clip_frames:
                clip_path = queue.next_path()
                size = encode_clip_mp4(
                    frame_buffer, clip_path,
                    args.fps, args.scale, args.crf
                )
                total_clips += 1
                size_kb = size / 1024
                queue_kb = queue.total_size_kb()
                pushes = pusher.push_count if pusher else 0
                print(f"[LIVE] clip #{total_clips} ({size_kb:.0f} KB) | "
                      f"queue: {queue_kb:.0f} KB | "
                      f"frames: {total_frames} | "
                      f"pushes: {pushes}")
                frame_buffer.clear()

        except Exception as e:
            print(f"[LIVE] Error: {e}")
            frame_buffer.clear()
            time.sleep(0.5)
            continue

        elapsed = time.perf_counter() - t0
        sleep_time = frame_time - elapsed
        if sleep_time > 0:
            time.sleep(sleep_time)

    # --- SHUTDOWN ---
    if frame_buffer:
        clip_path = queue.next_path()
        encode_clip_mp4(frame_buffer, clip_path, args.fps, args.scale, args.crf)
        print(f"[LIVE] Flushed {len(frame_buffer)} remaining frames")
        frame_buffer.clear()

    if pusher:
        pusher.stop()
        pusher.join(timeout=5)
        print("[LIVE] Final push...")
        pusher.final_push()
        if not args.no_squash:
            pusher.cleanup_commits()

    if cap:
        cap.release()
    if sct:
        sct.close()

    pushes = pusher.push_count if pusher else 0
    print(f"[LIVE] Done — {total_clips} clips, {total_frames} frames, {pushes} pushes")


if __name__ == "__main__":
    main()
