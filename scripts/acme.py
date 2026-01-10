#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Minimal ACME v2 client (HTTP-01) using only Python stdlib + openssl CLI.

Usage (staging recommended first):
  python3 scripts/acme.py \
    --domain example.com \
    --email you@example.com \
    --challenge-dir /var/www/acme-challenge \
    --out-dir ./certs \
    --staging

Proxy integration:
  Set config:
    [tls]
    acme_challenge_dir = /var/www/acme-challenge
  Then proxy serves:
    /.well-known/acme-challenge/<token>

Notes:
  - This script is best-effort and intentionally dependency-minimal.
  - For production, ensure the domain resolves to this host and HTTP is reachable.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Tuple


LE_DIR = "https://acme-v02.api.letsencrypt.org/directory"
LE_STAGING_DIR = "https://acme-staging-v02.api.letsencrypt.org/directory"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def json_b64url(obj: Any) -> str:
    return b64url(json.dumps(obj, sort_keys=True, separators=(",", ":")).encode("utf-8"))


def run(cmd: list[str], input_bytes: Optional[bytes] = None) -> bytes:
    p = subprocess.run(cmd, input=input_bytes, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if p.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(cmd)}\n{p.stderr.decode('utf-8', 'ignore')}")
    return p.stdout


def ensure_openssl() -> None:
    try:
        run(["openssl", "version"])
    except Exception as e:
        raise SystemExit(f"openssl not available: {e}")


def gen_rsa_key(path: Path, bits: int = 2048) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        return
    run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", f"rsa_keygen_bits:{bits}", "-out", str(path)])
    os.chmod(path, 0o600)


def rsa_jwk_from_key(key_path: Path) -> Dict[str, str]:
    # modulus hex
    out = run(["openssl", "rsa", "-in", str(key_path), "-noout", "-modulus"]).decode().strip()
    # Modulus=ABCDEF...
    if "Modulus=" not in out:
        raise RuntimeError("failed to get RSA modulus")
    mod_hex = out.split("Modulus=")[1].strip()
    n = b64url(bytes.fromhex(mod_hex))

    # exponent
    txt = run(["openssl", "rsa", "-in", str(key_path), "-text", "-noout"]).decode("utf-8", "ignore")
    e_val = 65537
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("publicExponent:"):
            # publicExponent: 65537 (0x10001)
            parts = line.split()
            if len(parts) >= 2:
                try:
                    e_val = int(parts[1])
                except Exception:
                    pass
            break
    e = b64url(e_val.to_bytes((e_val.bit_length() + 7) // 8, "big"))
    return {"kty": "RSA", "n": n, "e": e}


def jwk_thumbprint(jwk: Dict[str, str]) -> str:
    canon = json.dumps(jwk, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return b64url(hashlib.sha256(canon).digest())


def sign_rs256(key_path: Path, data: bytes) -> bytes:
    return run(["openssl", "dgst", "-sha256", "-sign", str(key_path)], input_bytes=data)


@dataclass
class AcmeDirectory:
    newNonce: str
    newAccount: str
    newOrder: str


class AcmeClient:
    def __init__(self, directory_url: str, account_key: Path, kid_path: Path):
        self.directory_url = directory_url
        self.account_key = account_key
        self.kid_path = kid_path
        self._dir: Optional[AcmeDirectory] = None
        self._nonce: Optional[str] = None
        self._jwk = rsa_jwk_from_key(account_key)
        self._thumb = jwk_thumbprint(self._jwk)

    def _http(self, req: urllib.request.Request) -> Tuple[int, Dict[str, str], bytes]:
        with urllib.request.urlopen(req, timeout=15) as r:
            code = r.getcode()
            headers = {k.lower(): v for k, v in r.headers.items()}
            body = r.read()
            return code, headers, body

    def _head_nonce(self) -> str:
        assert self._dir is not None
        req = urllib.request.Request(self._dir.newNonce, method="HEAD")
        code, headers, _ = self._http(req)
        if code not in (200, 204):
            raise RuntimeError(f"newNonce failed: {code}")
        nonce = headers.get("replay-nonce")
        if not nonce:
            raise RuntimeError("missing replay-nonce")
        return nonce

    def directory(self) -> AcmeDirectory:
        if self._dir is not None:
            return self._dir
        req = urllib.request.Request(self.directory_url, headers={"Accept": "application/json"})
        code, _, body = self._http(req)
        if code != 200:
            raise RuntimeError(f"directory failed: {code}")
        j = json.loads(body.decode("utf-8"))
        self._dir = AcmeDirectory(newNonce=j["newNonce"], newAccount=j["newAccount"], newOrder=j["newOrder"])
        self._nonce = self._head_nonce()
        return self._dir

    def _kid(self) -> Optional[str]:
        if self.kid_path.exists():
            v = self.kid_path.read_text(encoding="utf-8").strip()
            return v or None
        return None

    def _save_kid(self, kid: str) -> None:
        self.kid_path.parent.mkdir(parents=True, exist_ok=True)
        self.kid_path.write_text(kid + "\n", encoding="utf-8")

    def _jws(self, url: str, payload: Any, use_kid: bool) -> bytes:
        if self._nonce is None:
            self._nonce = self._head_nonce()
        protected: Dict[str, Any] = {"alg": "RS256", "nonce": self._nonce, "url": url}
        if use_kid:
            kid = self._kid()
            if not kid:
                raise RuntimeError("no account kid available")
            protected["kid"] = kid
        else:
            protected["jwk"] = self._jwk

        p64 = json_b64url(protected)
        pl64 = "" if payload == "" else json_b64url(payload)
        signing_input = (p64 + "." + pl64).encode("ascii")
        sig = b64url(sign_rs256(self.account_key, signing_input))
        body = json.dumps({"protected": p64, "payload": pl64, "signature": sig}).encode("utf-8")
        return body

    def post(self, url: str, payload: Any, use_kid: bool) -> Tuple[int, Dict[str, str], Any]:
        body = self._jws(url, payload, use_kid=use_kid)
        req = urllib.request.Request(url, data=body, method="POST", headers={"Content-Type": "application/jose+json"})
        code, headers, resp = self._http(req)
        self._nonce = headers.get("replay-nonce", self._nonce)
        if resp:
            return code, headers, json.loads(resp.decode("utf-8"))
        return code, headers, {}

    def get_json(self, url: str) -> Tuple[int, Dict[str, str], Any]:
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        code, headers, body = self._http(req)
        if body:
            return code, headers, json.loads(body.decode("utf-8"))
        return code, headers, {}

    def get_pem(self, url: str) -> str:
        req = urllib.request.Request(url, headers={"Accept": "application/pem-certificate-chain"})
        code, _, body = self._http(req)
        if code != 200:
            raise RuntimeError(f"cert download failed: {code}")
        return body.decode("utf-8")

    def ensure_account(self, email: str) -> None:
        d = self.directory()
        if self._kid():
            return
        payload = {"termsOfServiceAgreed": True, "contact": [f"mailto:{email}"]}
        code, headers, _ = self.post(d.newAccount, payload, use_kid=False)
        if code not in (200, 201):
            raise RuntimeError(f"newAccount failed: {code}")
        kid = headers.get("location")
        if not kid:
            raise RuntimeError("missing account Location header")
        self._save_kid(kid)

    def http01_key_authorization(self, token: str) -> str:
        return token + "." + self._thumb


def build_csr_der(domain_key: Path, domain: str, csr_der: Path) -> bytes:
    tmp_csr = csr_der.with_suffix(".pem")
    # OpenSSL 1.1.1+: -addext
    try:
        run(
            [
                "openssl",
                "req",
                "-new",
                "-key",
                str(domain_key),
                "-subj",
                f"/CN={domain}",
                "-addext",
                f"subjectAltName=DNS:{domain}",
                "-out",
                str(tmp_csr),
            ]
        )
    except Exception:
        # Fallback config file.
        cfg = csr_der.with_suffix(".cnf")
        cfg.write_text(
            "\n".join(
                [
                    "[req]",
                    "prompt = no",
                    "distinguished_name = dn",
                    "req_extensions = v3_req",
                    "[dn]",
                    f"CN = {domain}",
                    "[v3_req]",
                    f"subjectAltName = DNS:{domain}",
                ]
            ),
            encoding="utf-8",
        )
        run(["openssl", "req", "-new", "-key", str(domain_key), "-config", str(cfg), "-out", str(tmp_csr)])
        try:
            cfg.unlink()
        except Exception:
            pass

    der = run(["openssl", "req", "-in", str(tmp_csr), "-outform", "DER"])
    csr_der.write_bytes(der)
    try:
        tmp_csr.unlink()
    except Exception:
        pass
    return der


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--domain", required=True)
    ap.add_argument("--email", required=True)
    ap.add_argument("--challenge-dir", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--staging", action="store_true")
    ap.add_argument("--poll-interval", type=float, default=1.0)
    ap.add_argument("--poll-timeout", type=float, default=120.0)
    args = ap.parse_args()

    ensure_openssl()
    domain = args.domain.strip()
    email = args.email.strip()
    challenge_dir = Path(args.challenge_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    challenge_dir.mkdir(parents=True, exist_ok=True)

    dir_url = LE_STAGING_DIR if args.staging else LE_DIR

    account_key = out_dir / "account.key.pem"
    kid_path = out_dir / "account.kid"
    gen_rsa_key(account_key, 2048)

    c = AcmeClient(dir_url, account_key, kid_path)
    d = c.directory()
    c.ensure_account(email=email)

    # New order
    code, headers, order = c.post(d.newOrder, {"identifiers": [{"type": "dns", "value": domain}]}, use_kid=True)
    if code not in (201, 200):
        raise RuntimeError(f"newOrder failed: {code} {order}")
    order_url = headers.get("location")
    if not order_url:
        raise RuntimeError("missing order Location header")
    finalize = order["finalize"]
    authz_url = order["authorizations"][0]

    # Authorization
    _, _, authz = c.get_json(authz_url)
    if authz.get("status") == "valid":
        pass
    else:
        chall = None
        for ch in authz.get("challenges", []):
            if ch.get("type") == "http-01":
                chall = ch
                break
        if not chall:
            raise RuntimeError("no http-01 challenge found")
        token = chall["token"]
        key_auth = c.http01_key_authorization(token)
        token_path = challenge_dir / token
        token_path.write_text(key_auth, encoding="utf-8")

        # Notify challenge
        c.post(chall["url"], {}, use_kid=True)

        # Poll
        start = time.time()
        while time.time() - start < args.poll_timeout:
            _, _, authz2 = c.get_json(authz_url)
            st = authz2.get("status")
            if st == "valid":
                break
            if st == "invalid":
                raise RuntimeError(f"authorization invalid: {authz2}")
            time.sleep(args.poll_interval)
        else:
            raise RuntimeError("authorization poll timeout")

    # CSR + finalize
    domain_key = out_dir / f"{domain}.key.pem"
    gen_rsa_key(domain_key, 2048)
    csr_der_path = out_dir / f"{domain}.csr.der"
    csr_der = build_csr_der(domain_key, domain, csr_der_path)
    csr_b64 = b64url(csr_der)
    c.post(finalize, {"csr": csr_b64}, use_kid=True)

    # Poll order (POST-as-GET) until certificate URL is available.
    start = time.time()
    cert_url = None
    while time.time() - start < args.poll_timeout:
        # POST-as-GET to order URL
        code, _, oj = c.post(order_url, "", use_kid=True)
        if code not in (200, 201):
            time.sleep(args.poll_interval)
            continue
        if oj.get("status") == "valid" and oj.get("certificate"):
            cert_url = oj["certificate"]
            break
        if oj.get("status") == "invalid":
            raise RuntimeError(f"order invalid: {oj}")
        time.sleep(args.poll_interval)
    if not cert_url:
        raise RuntimeError("order poll timeout (certificate url not available)")

    pem = c.get_pem(cert_url)
    (out_dir / "fullchain.pem").write_text(pem, encoding="utf-8")
    (out_dir / "privkey.pem").write_text(domain_key.read_text(encoding="utf-8"), encoding="utf-8")

    print("OK")
    print(f"fullchain: {out_dir / 'fullchain.pem'}")
    print(f"privkey:   {out_dir / 'privkey.pem'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
