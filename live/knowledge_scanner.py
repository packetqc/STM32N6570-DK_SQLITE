#!/usr/bin/env python3
"""Knowledge Scanner — PQC Discovery Protocol v0

Scans the container subnet for knowledge beacons. Used by satellite
instances on wakeup to discover the core, or by any instance to map
the live network.

Well-known port: 21337
Protocol: connect → beacon sends identity → scanner sends identity → close

Usage:
    python3 knowledge_scanner.py                   # scan local subnet
    python3 knowledge_scanner.py --subnet 21.0.0.0/25  # explicit subnet
    python3 knowledge_scanner.py --port 21337      # custom port
    python3 knowledge_scanner.py --connect 21.0.0.38    # direct connect
    python3 knowledge_scanner.py --json            # JSON output for piping

Part of the live-knowledge toolset — synced from packetqc/knowledge.
"""

import socket
import json
import time
import os
import sys
import argparse
import ipaddress
import subprocess
import concurrent.futures

BEACON_PORT = 21337
PROTOCOL_VERSION = "pqc-discovery-v0"
SCAN_TIMEOUT = 0.3
SCAN_THREADS = 64
PEERS_FILE = "/tmp/knowledge_peers.json"


def get_ip():
    """Get container's IP address."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "0.0.0.0"


def get_subnet():
    """Detect the container's subnet (CIDR)."""
    try:
        ip = get_ip()
        network = ipaddress.IPv4Network(f"{ip}/25", strict=False)
        return network
    except Exception:
        return None


def detect_repo():
    """Detect the current repo name from git remote."""
    try:
        result = subprocess.run(
            ["git", "remote", "get-url", "origin"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            url = result.stdout.strip()
            for prefix in ["https://github.com/", "git@github.com:"]:
                if url.startswith(prefix):
                    return url[len(prefix):].rstrip(".git")
            if "/git/" in url:
                return url.split("/git/")[-1].rstrip(".git")
    except Exception:
        pass
    return os.path.basename(os.getcwd())


def detect_branch():
    """Detect the current git branch."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return "unknown"


def build_identity():
    """Build scanner identity payload."""
    return {
        "type": "knowledge-scanner",
        "repo": detect_repo(),
        "branch": detect_branch(),
        "ip": get_ip(),
        "port": BEACON_PORT,
        "protocol": PROTOCOL_VERSION,
        "role": "satellite",
        "status": "scanning",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "pid": os.getpid()
    }


def probe_host(ip_str, port, timeout, my_identity):
    """Probe a single host for a knowledge beacon. Returns peer identity or None."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect((ip_str, port))
        # Read beacon identity
        data = s.recv(4096)
        if data:
            peer = json.loads(data.decode("utf-8"))
            # Send our identity back
            s.sendall(json.dumps(my_identity).encode("utf-8") + b"\n")
            s.close()
            peer["_discovered_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            peer["_discovered_by"] = my_identity["repo"]
            return peer
        s.close()
    except (socket.timeout, ConnectionRefusedError, OSError, json.JSONDecodeError):
        pass
    return None


def save_peers(peers):
    """Save discovered peers to JSON file."""
    existing = {}
    if os.path.isfile(PEERS_FILE):
        try:
            with open(PEERS_FILE, "r") as f:
                existing = json.load(f)
        except Exception:
            pass
    for peer in peers:
        key = f"{peer.get('ip', '?')}:{peer.get('port', '?')}"
        existing[key] = peer
    with open(PEERS_FILE, "w") as f:
        json.dump(existing, f, indent=2)


def scan_subnet(subnet=None, port=BEACON_PORT, timeout=SCAN_TIMEOUT, json_output=False):
    """Scan subnet for knowledge beacons."""
    if subnet is None:
        subnet = get_subnet()
    if subnet is None:
        print("ERROR: Cannot detect subnet", file=sys.stderr)
        return []

    my_ip = get_ip()
    my_identity = build_identity()
    hosts = [str(ip) for ip in subnet.hosts() if str(ip) != my_ip]

    if not json_output:
        print(f"Knowledge Network Scanner — {PROTOCOL_VERSION}")
        print(f"  Self: {my_identity['repo']} @ {my_ip}")
        print(f"  Scanning {len(hosts)} hosts on {subnet} port {port}...")
        print()

    peers = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=SCAN_THREADS) as pool:
        futures = {pool.submit(probe_host, ip, port, timeout, my_identity): ip for ip in hosts}
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            if result is not None:
                peers.append(result)
                if not json_output:
                    role = result.get("role", "?")
                    repo = result.get("repo", "?")
                    ip = result.get("ip", "?")
                    port_r = result.get("port", "?")
                    branch = result.get("branch", "?")
                    conns = result.get("connections", "?")
                    print(f"  FOUND: [{role}] {repo}")
                    print(f"         {ip}:{port_r} branch={branch} connections={conns}")

    if peers:
        save_peers(peers)

    if not json_output:
        print()
        if peers:
            print(f"  {len(peers)} knowledge beacon(s) found")
            print(f"  Peers saved to {PEERS_FILE}")
        else:
            print("  No knowledge beacons found on subnet")
            print("  (Is the core session running with beacon active?)")
    return peers


def direct_connect(host, port=BEACON_PORT, json_output=False):
    """Connect directly to a known beacon."""
    my_identity = build_identity()
    if not json_output:
        print(f"Connecting to {host}:{port}...")

    peer = probe_host(host, port, timeout=3.0, my_identity=my_identity)
    if peer:
        if not json_output:
            print(f"  Connected: [{peer.get('role', '?')}] {peer.get('repo', '?')}")
            print(f"  Branch: {peer.get('branch', '?')}")
            print(f"  Protocol: {peer.get('protocol', '?')}")
            print(f"  Connections: {peer.get('connections', '?')}")
            print(f"  Started: {peer.get('started', '?')}")
        save_peers([peer])
        return peer
    else:
        if not json_output:
            print(f"  FAILED: No beacon at {host}:{port}")
        return None


def main():
    parser = argparse.ArgumentParser(description="Knowledge Scanner — PQC Discovery Protocol v0")
    parser.add_argument("--subnet", type=str, default=None,
                        help="Subnet to scan (CIDR, default: auto-detect)")
    parser.add_argument("--port", type=int, default=BEACON_PORT,
                        help=f"Beacon port (default: {BEACON_PORT})")
    parser.add_argument("--connect", type=str, default=None,
                        help="Direct connect to a known beacon IP")
    parser.add_argument("--json", action="store_true",
                        help="JSON output for piping")
    parser.add_argument("--timeout", type=float, default=SCAN_TIMEOUT,
                        help=f"Per-host timeout in seconds (default: {SCAN_TIMEOUT})")
    args = parser.parse_args()

    # Direct connect mode
    if args.connect:
        peer = direct_connect(args.connect, port=args.port, json_output=args.json)
        if args.json and peer:
            print(json.dumps(peer, indent=2))
        sys.exit(0 if peer else 1)

    # Subnet scan mode
    subnet = None
    if args.subnet:
        try:
            subnet = ipaddress.IPv4Network(args.subnet, strict=False)
        except ValueError as e:
            print(f"ERROR: Invalid subnet: {e}", file=sys.stderr)
            sys.exit(1)

    peers = scan_subnet(subnet=subnet, port=args.port, timeout=args.timeout, json_output=args.json)
    if args.json:
        print(json.dumps(peers, indent=2))
    sys.exit(0 if peers else 1)


if __name__ == "__main__":
    main()
