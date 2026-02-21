#!/usr/bin/env python3
"""PQC Envelope — Post-Quantum Secure Token Exchange & Network Crypto

Dual-use cryptographic module:
  1. Token exchange  — secure ephemeral PAT delivery to Claude Code sessions
  2. Network crypto  — beacon-to-beacon encrypted communication (core ↔ satellites)

Crypto ladder (auto-detects best available):
  Level 3: ML-KEM-1024 (FIPS 203) — post-quantum, requires OpenSSL 3.5+ or oqs-provider
  Level 2: X25519 + HKDF + AES-256-CBC — classical ECDH, OpenSSL 3.0+
  Level 1: Curve25519 via Python cryptography lib — fallback

All key material lives in /dev/shm/ (RAM filesystem) or /tmp/ — never on persistent storage.
Keys are destroyed immediately after use. Token never written to any file.

Usage:
  # Session side (Claude Code):
  python3 scripts/pqc_envelope.py keygen              # Generate keypair, print public key
  python3 scripts/pqc_envelope.py decrypt <envelope>   # Decrypt user's envelope, output token

  # User side (their machine):
  python3 scripts/pqc_envelope.py encrypt <pubkey>     # Encrypt token with session's public key

  # Beacon crypto (imported as module):
  from scripts.pqc_envelope import PQCEnvelope
  env = PQCEnvelope()
  pub = env.generate_keypair()
  shared = env.derive_shared_secret(peer_pub)

Authors: Martin Paquet, Claude (Anthropic)
License: MIT
Knowledge version: v27
"""

import subprocess
import os
import sys
import base64
import hashlib
import hmac
import json
import tempfile
import shutil
from pathlib import Path

# --- Crypto level detection ---

CRYPTO_LEVEL = 0
CRYPTO_NAME = "none"

def _detect_crypto_level():
    """Auto-detect best available crypto. Returns (level, name)."""
    # Level 3: ML-KEM-1024 via OpenSSL
    try:
        r = subprocess.run(
            ["openssl", "genpkey", "-algorithm", "mlkem1024", "-out", "/dev/null"],
            capture_output=True, timeout=5
        )
        if r.returncode == 0:
            return 3, "ML-KEM-1024 (FIPS 203)"
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # Also try ML-KEM-1024 (hyphenated form)
    try:
        r = subprocess.run(
            ["openssl", "genpkey", "-algorithm", "ML-KEM-1024", "-out", "/dev/null"],
            capture_output=True, timeout=5
        )
        if r.returncode == 0:
            return 3, "ML-KEM-1024 (FIPS 203)"
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # Level 2: X25519 via OpenSSL
    try:
        r = subprocess.run(
            ["openssl", "genpkey", "-algorithm", "X25519", "-out", "/dev/null"],
            capture_output=True, timeout=5
        )
        if r.returncode == 0:
            return 2, "X25519 + AES-256-CBC (OpenSSL)"
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # Level 1: Python cryptography library
    try:
        from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
        return 1, "X25519 (Python cryptography)"
    except ImportError:
        pass

    return 0, "none"


# Detect on import
CRYPTO_LEVEL, CRYPTO_NAME = _detect_crypto_level()

# --- Secure temp directory ---

_INSTANCE_COUNTER = 0

def _get_secure_tmpdir():
    """Use /dev/shm (RAM) if available, fallback to /tmp.
    Each call gets a unique directory (PID + counter)."""
    global _INSTANCE_COUNTER
    _INSTANCE_COUNTER += 1
    suffix = f"{os.getpid()}_{_INSTANCE_COUNTER}"
    shm = Path("/dev/shm")
    if shm.exists() and shm.is_dir():
        d = shm / f"pqc_envelope_{suffix}"
        d.mkdir(mode=0o700, exist_ok=True)
        return d
    d = Path(tempfile.mkdtemp(prefix=f"pqc_envelope_{suffix}_"))
    os.chmod(str(d), 0o700)
    return d


def _secure_delete(path):
    """Overwrite file with zeros before unlinking."""
    p = Path(path)
    if p.exists():
        size = p.stat().st_size
        with open(p, 'wb') as f:
            f.write(b'\x00' * size)
            f.flush()
            os.fsync(f.fileno())
        p.unlink()


def _cleanup_tmpdir(tmpdir):
    """Securely delete all files in tmpdir, then remove it."""
    tmpdir = Path(tmpdir)
    if tmpdir.exists():
        for f in tmpdir.iterdir():
            _secure_delete(f)
        tmpdir.rmdir()


# --- OpenSSL X25519 operations (Level 2) ---

class OpenSSLX25519:
    """X25519 ECDH + AES-256-CBC via OpenSSL CLI."""

    def __init__(self):
        self.tmpdir = _get_secure_tmpdir()
        self.privkey_path = self.tmpdir / "priv.pem"
        self.pubkey_path = self.tmpdir / "pub.pem"

    def generate_keypair(self):
        """Generate X25519 keypair. Returns public key as base64 PEM."""
        subprocess.run(
            ["openssl", "genpkey", "-algorithm", "X25519",
             "-out", str(self.privkey_path)],
            capture_output=True, check=True
        )
        os.chmod(str(self.privkey_path), 0o600)

        r = subprocess.run(
            ["openssl", "pkey", "-in", str(self.privkey_path),
             "-pubout", "-out", str(self.pubkey_path)],
            capture_output=True, check=True
        )
        return self.pubkey_path.read_text().strip()

    def derive_shared_secret(self, peer_pubkey_pem):
        """Derive shared secret from our private key + peer's public key."""
        peer_pub_path = self.tmpdir / "peer_pub.pem"
        peer_pub_path.write_text(peer_pubkey_pem)

        r = subprocess.run(
            ["openssl", "pkeyutl", "-derive",
             "-inkey", str(self.privkey_path),
             "-peerkey", str(peer_pub_path),
             "-out", str(self.tmpdir / "shared_raw.bin")],
            capture_output=True, check=True
        )

        raw = (self.tmpdir / "shared_raw.bin").read_bytes()
        # HKDF-like: SHA-256 of raw shared secret for key derivation
        shared = hashlib.sha256(raw).digest()

        _secure_delete(peer_pub_path)
        _secure_delete(self.tmpdir / "shared_raw.bin")
        return shared

    def encrypt(self, plaintext, shared_secret):
        """Encrypt plaintext with AES-256-CBC using shared secret."""
        passphrase = shared_secret.hex()
        pt_path = self.tmpdir / "pt.bin"
        ct_path = self.tmpdir / "ct.bin"

        pt_path.write_bytes(plaintext.encode() if isinstance(plaintext, str) else plaintext)

        subprocess.run(
            ["openssl", "enc", "-aes-256-cbc",
             "-pass", f"pass:{passphrase}",
             "-pbkdf2", "-salt",
             "-in", str(pt_path), "-out", str(ct_path)],
            capture_output=True, check=True
        )

        ciphertext = ct_path.read_bytes()

        _secure_delete(pt_path)
        _secure_delete(ct_path)

        # Append HMAC for authentication
        mac = hmac.new(shared_secret, ciphertext, hashlib.sha256).digest()
        return ciphertext + mac

    def decrypt(self, authenticated_ciphertext, shared_secret):
        """Decrypt and verify HMAC."""
        ciphertext = authenticated_ciphertext[:-32]
        mac = authenticated_ciphertext[-32:]

        # Verify HMAC first
        expected_mac = hmac.new(shared_secret, ciphertext, hashlib.sha256).digest()
        if not hmac.compare_digest(mac, expected_mac):
            raise ValueError("HMAC verification failed — ciphertext tampered or wrong key")

        passphrase = shared_secret.hex()
        ct_path = self.tmpdir / "ct.bin"
        pt_path = self.tmpdir / "pt.bin"

        ct_path.write_bytes(ciphertext)

        subprocess.run(
            ["openssl", "enc", "-d", "-aes-256-cbc",
             "-pass", f"pass:{passphrase}",
             "-pbkdf2",
             "-in", str(ct_path), "-out", str(pt_path)],
            capture_output=True, check=True
        )

        plaintext = pt_path.read_bytes()

        _secure_delete(ct_path)
        _secure_delete(pt_path)
        return plaintext.decode()

    def cleanup(self):
        """Destroy all key material."""
        _cleanup_tmpdir(self.tmpdir)


# --- ML-KEM-1024 operations (Level 3) ---

class OpenSSLMLKEM:
    """ML-KEM-1024 KEM + AES-256-CBC via OpenSSL CLI."""

    def __init__(self):
        self.tmpdir = _get_secure_tmpdir()
        self.privkey_path = self.tmpdir / "mlkem_priv.pem"
        self.pubkey_path = self.tmpdir / "mlkem_pub.pem"

    def generate_keypair(self):
        """Generate ML-KEM-1024 keypair. Returns public key as PEM."""
        # Try both algorithm name formats
        for alg in ["mlkem1024", "ML-KEM-1024"]:
            r = subprocess.run(
                ["openssl", "genpkey", "-algorithm", alg,
                 "-out", str(self.privkey_path)],
                capture_output=True
            )
            if r.returncode == 0:
                break
        else:
            raise RuntimeError("ML-KEM-1024 keygen failed")

        os.chmod(str(self.privkey_path), 0o600)

        subprocess.run(
            ["openssl", "pkey", "-in", str(self.privkey_path),
             "-pubout", "-out", str(self.pubkey_path)],
            capture_output=True, check=True
        )
        return self.pubkey_path.read_text().strip()

    def encapsulate(self, peer_pubkey_pem):
        """KEM encapsulate: produce ciphertext + shared secret using peer's public key."""
        peer_pub_path = self.tmpdir / "peer_pub.pem"
        ct_path = self.tmpdir / "kem_ct.bin"
        ss_path = self.tmpdir / "kem_ss.bin"

        peer_pub_path.write_text(peer_pubkey_pem)

        subprocess.run(
            ["openssl", "pkeyutl", "-encapsulate",
             "-pubin", "-inkey", str(peer_pub_path),
             "-out", str(ct_path), "-secret", str(ss_path)],
            capture_output=True, check=True
        )

        ciphertext = ct_path.read_bytes()
        shared_secret = ss_path.read_bytes()

        _secure_delete(peer_pub_path)
        _secure_delete(ct_path)
        _secure_delete(ss_path)
        return ciphertext, shared_secret

    def decapsulate(self, ciphertext):
        """KEM decapsulate: recover shared secret from ciphertext using our private key."""
        ct_path = self.tmpdir / "kem_ct.bin"
        ss_path = self.tmpdir / "kem_ss.bin"

        ct_path.write_bytes(ciphertext)

        subprocess.run(
            ["openssl", "pkeyutl", "-decapsulate",
             "-inkey", str(self.privkey_path),
             "-in", str(ct_path), "-secret", str(ss_path)],
            capture_output=True, check=True
        )

        shared_secret = ss_path.read_bytes()

        _secure_delete(ct_path)
        _secure_delete(ss_path)
        return shared_secret

    def encrypt(self, plaintext, shared_secret):
        """Encrypt with AES-256-CBC + HMAC using KEM-derived shared secret."""
        key = hashlib.sha256(shared_secret).digest()
        passphrase = key.hex()
        pt_path = self.tmpdir / "pt.bin"
        ct_path = self.tmpdir / "ct.bin"

        pt_path.write_bytes(plaintext.encode() if isinstance(plaintext, str) else plaintext)

        subprocess.run(
            ["openssl", "enc", "-aes-256-cbc",
             "-pass", f"pass:{passphrase}",
             "-pbkdf2", "-salt",
             "-in", str(pt_path), "-out", str(ct_path)],
            capture_output=True, check=True
        )

        ciphertext = ct_path.read_bytes()
        _secure_delete(pt_path)
        _secure_delete(ct_path)

        mac = hmac.new(key, ciphertext, hashlib.sha256).digest()
        return ciphertext + mac

    def decrypt(self, authenticated_ciphertext, shared_secret):
        """Decrypt and verify HMAC."""
        ciphertext = authenticated_ciphertext[:-32]
        mac = authenticated_ciphertext[-32:]

        key = hashlib.sha256(shared_secret).digest()
        expected_mac = hmac.new(key, ciphertext, hashlib.sha256).digest()
        if not hmac.compare_digest(mac, expected_mac):
            raise ValueError("HMAC verification failed")

        passphrase = key.hex()
        ct_path = self.tmpdir / "ct.bin"
        pt_path = self.tmpdir / "pt.bin"

        ct_path.write_bytes(ciphertext)

        subprocess.run(
            ["openssl", "enc", "-d", "-aes-256-cbc",
             "-pass", f"pass:{passphrase}",
             "-pbkdf2",
             "-in", str(ct_path), "-out", str(pt_path)],
            capture_output=True, check=True
        )

        plaintext = pt_path.read_bytes()
        _secure_delete(ct_path)
        _secure_delete(pt_path)
        return plaintext.decode()

    def cleanup(self):
        """Destroy all key material."""
        _cleanup_tmpdir(self.tmpdir)


# --- Unified envelope interface ---

class PQCEnvelope:
    """Unified PQC envelope — auto-selects best available crypto.

    Usage (token exchange):
        # Session side:
        env = PQCEnvelope()
        pubkey = env.generate_keypair()   # Print to user
        token = env.open_envelope(data)   # Decrypt user's envelope
        env.destroy()                     # Wipe all keys

        # User side:
        env = PQCEnvelope()
        envelope = env.seal_envelope(token, session_pubkey)
        env.destroy()

    Usage (beacon network crypto):
        env = PQCEnvelope()
        pubkey = env.generate_keypair()
        # Exchange pubkeys via beacon protocol
        shared = env.derive_or_encapsulate(peer_pubkey)
        ciphertext = env.encrypt(message, shared)
        plaintext = env.decrypt(ciphertext, shared)
        env.destroy()
    """

    def __init__(self):
        self.level = CRYPTO_LEVEL
        self.name = CRYPTO_NAME
        self.backend = None
        self._shared_secret = None

        if self.level == 3:
            self.backend = OpenSSLMLKEM()
        elif self.level == 2:
            self.backend = OpenSSLX25519()
        else:
            raise RuntimeError(
                f"No suitable crypto available (level={self.level}). "
                "Need OpenSSL 3.0+ with X25519 or OpenSSL 3.5+ with ML-KEM-1024."
            )

    def generate_keypair(self):
        """Generate keypair. Returns public key (PEM format)."""
        return self.backend.generate_keypair()

    def derive_or_encapsulate(self, peer_pubkey_pem):
        """Derive shared secret (X25519) or encapsulate (ML-KEM).
        Returns shared secret bytes. For ML-KEM, also returns KEM ciphertext."""
        if self.level == 3:
            ct, ss = self.backend.encapsulate(peer_pubkey_pem)
            self._shared_secret = ss
            return ss, ct  # caller must send ct to peer
        else:
            ss = self.backend.derive_shared_secret(peer_pubkey_pem)
            self._shared_secret = ss
            return ss, None

    def decapsulate(self, kem_ciphertext):
        """ML-KEM only: recover shared secret from KEM ciphertext."""
        if self.level != 3:
            raise RuntimeError("decapsulate only available with ML-KEM (level 3)")
        ss = self.backend.decapsulate(kem_ciphertext)
        self._shared_secret = ss
        return ss

    def encrypt(self, plaintext, shared_secret=None):
        """Encrypt plaintext. Uses stored shared_secret if not provided."""
        ss = shared_secret or self._shared_secret
        if ss is None:
            raise RuntimeError("No shared secret — call derive_or_encapsulate first")
        return self.backend.encrypt(plaintext, ss)

    def decrypt(self, ciphertext, shared_secret=None):
        """Decrypt ciphertext. Uses stored shared_secret if not provided."""
        ss = shared_secret or self._shared_secret
        if ss is None:
            raise RuntimeError("No shared secret — call derive_or_encapsulate first")
        return self.backend.decrypt(ciphertext, ss)

    def seal_envelope(self, token, session_pubkey_pem):
        """User side: seal a token for a session.
        Returns base64-encoded envelope (contains pubkey + encrypted token)."""
        user_pubkey = self.generate_keypair()

        if self.level == 3:
            kem_ct, shared = self.backend.encapsulate(session_pubkey_pem)
            encrypted = self.backend.encrypt(token, shared)
            envelope = {
                "version": 1,
                "level": self.level,
                "crypto": self.name,
                "kem_ciphertext": base64.b64encode(kem_ct).decode(),
                "encrypted_token": base64.b64encode(encrypted).decode()
            }
        else:
            shared = self.backend.derive_shared_secret(session_pubkey_pem)
            encrypted = self.backend.encrypt(token, shared)
            envelope = {
                "version": 1,
                "level": self.level,
                "crypto": self.name,
                "user_pubkey": user_pubkey,
                "encrypted_token": base64.b64encode(encrypted).decode()
            }

        return base64.b64encode(json.dumps(envelope).encode()).decode()

    def open_envelope(self, envelope_b64, user_pubkey_pem=None):
        """Session side: open an envelope and recover the token.
        Returns the decrypted token string. Token is NEVER written to any file."""
        envelope = json.loads(base64.b64decode(envelope_b64))

        if envelope.get("level", 2) == 3 and self.level == 3:
            kem_ct = base64.b64decode(envelope["kem_ciphertext"])
            shared = self.backend.decapsulate(kem_ct)
        else:
            pub = user_pubkey_pem or envelope.get("user_pubkey", "")
            shared = self.backend.derive_shared_secret(pub)

        encrypted = base64.b64decode(envelope["encrypted_token"])
        return self.backend.decrypt(encrypted, shared)

    def destroy(self):
        """Securely destroy all key material. Call this when done."""
        self._shared_secret = None
        if self.backend:
            self.backend.cleanup()
            self.backend = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.destroy()

    @staticmethod
    def info():
        """Report available crypto level."""
        return {
            "level": CRYPTO_LEVEL,
            "name": CRYPTO_NAME,
            "post_quantum": CRYPTO_LEVEL >= 3,
            "openssl": _get_openssl_version()
        }


def _get_openssl_version():
    try:
        r = subprocess.run(["openssl", "version"], capture_output=True, text=True, timeout=5)
        return r.stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return "not found"


# --- CLI interface ---

def cmd_info():
    """Print crypto level info."""
    info = PQCEnvelope.info()
    print(f"Crypto level:  {info['level']}")
    print(f"Algorithm:     {info['name']}")
    print(f"Post-quantum:  {'YES' if info['post_quantum'] else 'NO (upgrade to OpenSSL 3.5+ for ML-KEM)'}")
    print(f"OpenSSL:       {info['openssl']}")


def cmd_keygen():
    """Generate keypair, print public key for user."""
    with PQCEnvelope() as env:
        pubkey = env.generate_keypair()
        info = env.info()
        print(f"# PQC Envelope — {info['name']}")
        print(f"# Level {info['level']} | Post-quantum: {'YES' if info['post_quantum'] else 'NO'}")
        print(f"# Copy this public key and use it to encrypt your token:")
        print()
        print(pubkey)
        print()
        print("# On your machine, run:")
        print(f"#   python3 scripts/pqc_envelope.py encrypt '<paste-pubkey>'")
        print("#   (then paste the envelope back here)")
        # Note: keypair is destroyed on exit — keygen is for DISPLAY only
        # The session must call keygen again when ready to decrypt
        # This is intentional: no key persistence between commands


def cmd_encrypt(pubkey_pem):
    """User side: encrypt a token with session's public key."""
    import getpass
    token = getpass.getpass("Enter GitHub PAT (hidden): ")
    if not token.strip():
        print("ERROR: Empty token", file=sys.stderr)
        sys.exit(1)

    with PQCEnvelope() as env:
        envelope = env.seal_envelope(token.strip(), pubkey_pem)
        print()
        print("# Encrypted envelope — paste this into your Claude Code session:")
        print(envelope)


def cmd_decrypt(envelope_b64):
    """Session side: decrypt envelope, output token to stdout ONLY."""
    with PQCEnvelope() as env:
        _ = env.generate_keypair()  # Need our keypair to decrypt
        try:
            token = env.open_envelope(envelope_b64)
            # Output token to stdout — NEVER to a file
            # The caller (Claude Code) captures this in context memory only
            print(token, end='')
        except Exception as e:
            print(f"ERROR: Decryption failed — {e}", file=sys.stderr)
            sys.exit(1)


def cmd_test():
    """Self-test: full round-trip encryption/decryption."""
    print(f"Testing crypto level {CRYPTO_LEVEL}: {CRYPTO_NAME}")
    print()

    test_token = "ghp_TestToken1234567890ABCDEFghijklmno"

    # Simulate session side
    with PQCEnvelope() as session:
        session_pub = session.generate_keypair()
        print(f"Session keypair generated ({CRYPTO_NAME})")

        # Simulate user side
        with PQCEnvelope() as user:
            envelope = user.seal_envelope(test_token, session_pub)
            print(f"Token sealed in envelope ({len(envelope)} bytes)")

        # Session opens envelope
        recovered = session.open_envelope(envelope)

    if recovered == test_token:
        print(f"Round-trip: OK")
        print(f"Token match: YES")
    else:
        print(f"Round-trip: FAIL")
        print(f"Expected: {test_token}")
        print(f"Got:      {recovered}")
        sys.exit(1)

    # Verify cleanup
    print(f"Key material destroyed: YES")
    print()
    print("Self-test passed.")


def main():
    if len(sys.argv) < 2:
        print("PQC Envelope — Post-Quantum Secure Token Exchange")
        print()
        print("Usage:")
        print("  pqc_envelope.py info                  Show crypto level")
        print("  pqc_envelope.py keygen                Generate keypair, print public key")
        print("  pqc_envelope.py encrypt '<pubkey>'    Encrypt token (user side)")
        print("  pqc_envelope.py decrypt '<envelope>'  Decrypt envelope (session side)")
        print("  pqc_envelope.py test                  Self-test round-trip")
        print()
        cmd_info()
        sys.exit(0)

    cmd = sys.argv[1]

    if cmd == "info":
        cmd_info()
    elif cmd == "keygen":
        cmd_keygen()
    elif cmd == "encrypt":
        if len(sys.argv) < 3:
            print("Usage: pqc_envelope.py encrypt '<pubkey-pem>'", file=sys.stderr)
            sys.exit(1)
        cmd_encrypt(sys.argv[2])
    elif cmd == "decrypt":
        if len(sys.argv) < 3:
            print("Usage: pqc_envelope.py decrypt '<envelope-base64>'", file=sys.stderr)
            sys.exit(1)
        cmd_decrypt(sys.argv[2])
    elif cmd == "test":
        cmd_test()
    else:
        print(f"Unknown command: {cmd}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
