"""
ptlicense.py - he thong cap phat key cho pt.Live (chay tren VPS).

- Key chi luu dang HMAC-SHA256 (khong luu key goc).
- Thoi gian het han tinh bang dong ho CUA SERVER, bat dau dem tu lan kich hoat dau tien.
- Moi lan kich hoat / lam moi, server tra ve 1 TOKEN co chu ky Ed25519. Client chi can public key
  de kiem tra token, nen chay duoc khi mat mang (trong han offline_until).

Chi can: Python 3.9+, thu vien `cryptography`, sqlite3 (co san).
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import time
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

DAY = 86400

# ---- Cau hinh chinh (doi o day) ---------------------------------------------------------------
PLANS = {"trial": 3, "m1": 30, "m6": 180, "y1": 365}   # so ngay; "custom" dung --days hoac --until
OFFLINE_DAYS = 7          # token con dung duoc bao lau khi khong gap server
DEFAULT_DEVICES = 1       # so may toi da / 1 key
DEFAULT_RESETS = 3        # so lan nguoi dung tu go may / 1 key (admin reset de cap lai)
CLOCK_SLACK = 300         # client cho phep dong ho lech 5 phut

# Chong spam 'Gui yeu cau' / 'Kich hoat' (server la nguon su that; app co gioi han tuong tu de khoa nut cho dep)
REQ_MIN_GAP = 60          # giay giua 2 yeu cau cua CUNG 1 may
REQ_MAX_PER_HOUR = 3      # so yeu cau / may / 1 gio
REQ_MAX_PER_DAY = 6       # so yeu cau / may / 24 gio
REQ_IP_MAX_PER_HOUR = 10  # so yeu cau / IP / 1 gio (chong gia hwid ngau nhien de lam day danh sach cho duyet)
NEWDEV_IP_MAX_PER_HOUR = 10   # so may MOI (hwid chua co trong bang devices) / IP / 1 gio, ca claim lan request
CLAIM_MIN_GAP = 3         # nut Kich hoat: toi thieu 3 giay giua 2 lan cua cung 1 may
CLAIM_MAX_PER_HOUR = 60   # ... va toi da 60 lan / may / gio (user cho duyet co the bam nhieu lan)
CLAIM_IP_MAX_PER_HOUR = 120
MAX_TRIALS_PER_IP = 5     # so lan nhan thu bang MA DUNG THU moi IP trong cua so duoi day (0 = khong gioi han)
TRIAL_IP_WINDOW_DAYS = 30

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"   # Crockford base32: khong co I L O U
_TRANS = str.maketrans({"O": "0", "I": "1", "L": "1"})


class LicenseError(Exception):
    def __init__(self, code: str, http: int = 403, message: str = "", retry_after: int = 0):
        super().__init__(code)
        self.code, self.http, self.message = code, http, message or code
        self.retry_after = int(retry_after)        # > 0 voi rate_limited: so giay phai doi


# ---- Key ---------------------------------------------------------------------------------------
def gen_key() -> str:
    body = "".join(secrets.choice(ALPHABET) for _ in range(20))          # 100 bit ngau nhien
    return "PTL-" + "-".join(body[i:i + 5] for i in range(0, 20, 5))


def normalize_key(text: str) -> str:
    s = "".join(ch for ch in text.upper() if ch.isalnum())
    if len(s) == 23 and s.startswith("PTL"):
        s = s[3:]
    s = s.translate(_TRANS)
    if len(s) != 20 or any(c not in ALPHABET for c in s):
        raise LicenseError("invalid_key", 404, "Key khong dung dinh dang")
    return s


def key_hash(secret: bytes, key: str) -> str:
    return hmac.new(secret, normalize_key(key).encode(), hashlib.sha256).hexdigest()


# ---- Chu ky / token ----------------------------------------------------------------------------
def _b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).decode().rstrip("=")


def _unb64u(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


class Signer:
    def __init__(self, priv: Ed25519PrivateKey):
        self.priv = priv
        self.pub = priv.public_key()

    @classmethod
    def load_or_create(cls, path: Path) -> "Signer":
        if path.exists():
            return cls(serialization.load_pem_private_key(path.read_bytes(), password=None))
        priv = Ed25519PrivateKey.generate()
        path.write_bytes(priv.private_bytes(serialization.Encoding.PEM,
                                            serialization.PrivateFormat.PKCS8,
                                            serialization.NoEncryption()))
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
        return cls(priv)

    def public_b64(self) -> str:
        """32 byte public key, base64 chuan: dan vao client."""
        raw = self.pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        return base64.b64encode(raw).decode()

    def sign(self, payload: dict) -> str:
        data = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode()
        return _b64u(data) + "." + _b64u(self.priv.sign(data))


def verify_token(pub: Ed25519PublicKey, token: str) -> Optional[dict]:
    """Tra ve payload neu chu ky dung, nguoc lai None. Client C++ lam dung nhu vay tren BYTE da giai ma."""
    try:
        data_b64, sig_b64 = token.split(".")
        data, sig = _unb64u(data_b64), _unb64u(sig_b64)
        pub.verify(sig, data)
        return json.loads(data)
    except (ValueError, InvalidSignature, json.JSONDecodeError):
        return None


# ---- Database ----------------------------------------------------------------------------------
SCHEMA = """
CREATE TABLE IF NOT EXISTS licenses (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    key_hash        TEXT NOT NULL UNIQUE,
    key_hint        TEXT NOT NULL,                 -- 4 ky tu cuoi, de admin nhan ra key
    plan            TEXT NOT NULL,                 -- trial | m1 | m6 | y1 | custom
    duration_days   INTEGER,                       -- dem tu luc kich hoat (NULL neu dung fixed_expires_at)
    fixed_expires_at INTEGER,                      -- ngay het han co dinh (epoch), uu tien hon duration_days
    activate_before INTEGER,                       -- phai kich hoat truoc moc nay (NULL = khong han che)
    max_devices     INTEGER NOT NULL DEFAULT 1,
    resets_left     INTEGER NOT NULL DEFAULT 3,
    status          TEXT NOT NULL DEFAULT 'unused',-- unused | active | revoked
    activated_at    INTEGER,
    expires_at      INTEGER,
    note            TEXT NOT NULL DEFAULT '',
    created_at      INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS activations (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    license_id  INTEGER NOT NULL REFERENCES licenses(id),
    hwid        TEXT NOT NULL,
    hw_label    TEXT NOT NULL DEFAULT '',
    app_version TEXT NOT NULL DEFAULT '',
    active      INTEGER NOT NULL DEFAULT 1,
    first_seen  INTEGER NOT NULL,
    last_seen   INTEGER NOT NULL,
    UNIQUE (license_id, hwid)
);
CREATE TABLE IF NOT EXISTS trial_codes (          -- ma cong khai (vd so dien thoai) de khach tu nhan dung thu
    code_hash  TEXT PRIMARY KEY,
    label      TEXT NOT NULL DEFAULT '',
    days       INTEGER NOT NULL DEFAULT 3,
    enabled    INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS trial_ips (            -- chi luu HMAC cua IP, khong luu IP that
    ip_hash TEXT NOT NULL,
    used_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_trial_ips ON trial_ips (ip_hash, used_at);
CREATE TABLE IF NOT EXISTS trial_hwids (          -- moi may chi duoc dung thu 1 lan
    hwid       TEXT PRIMARY KEY,
    license_id INTEGER NOT NULL,
    used_at    INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (               -- bang thiet bi cho claim flow (khong can key)
    hwid         TEXT PRIMARY KEY,                  -- 64 ky tu hex, chu thuong
    hw_label     TEXT,                              -- ten may app gui len
    status       TEXT NOT NULL DEFAULT 'pending',  -- pending | approved | revoked
    plan         TEXT,                              -- trial | m1 | m6 | y1 | custom
    approved_at  INTEGER,
    expires_at   INTEGER,                           -- epoch giay (het han)
    first_seen   INTEGER NOT NULL,
    last_seen    INTEGER NOT NULL,
    last_ip      TEXT,
    app_version  TEXT,
    phone        TEXT,                          -- so dien thoai user gui yeu cau (admin lien he)
    note         TEXT,
    requested_at INTEGER                        -- lan cuoi user bam 'Gui yeu cau' (NULL sau khi admin duyet)
);
CREATE TABLE IF NOT EXISTS request_log (           -- nhat ky chong spam (tu xoa sau 24 gio)
    kind TEXT NOT NULL,                             -- request | claim | newdev
    hwid TEXT NOT NULL,
    ip   TEXT NOT NULL DEFAULT '',
    ts   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_reqlog_hwid ON request_log (kind, hwid, ts);
CREATE INDEX IF NOT EXISTS idx_reqlog_ip   ON request_log (kind, ip, ts);
"""


def connect(path: str | Path) -> sqlite3.Connection:
    db = sqlite3.connect(str(path), isolation_level=None, timeout=10)   # tu quan ly BEGIN/COMMIT
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA journal_mode=WAL")
    db.execute("PRAGMA busy_timeout=5000")
    db.execute("PRAGMA foreign_keys=ON")
    db.executescript(SCHEMA)
    # migration: them cot phone cho bang devices (an toan neu da co)
    try:
        db.execute("ALTER TABLE devices ADD COLUMN phone TEXT")
    except sqlite3.OperationalError:
        pass
    try:
        db.execute("ALTER TABLE devices ADD COLUMN requested_at INTEGER")
    except sqlite3.OperationalError:
        pass
    return db


@contextmanager
def _tx(db: sqlite3.Connection):
    db.execute("BEGIN IMMEDIATE")
    try:
        yield
        db.execute("COMMIT")
    except BaseException:
        db.execute("ROLLBACK")
        raise


def _now(now: Optional[int]) -> int:
    return int(time.time()) if now is None else int(now)


def _check_hwid(hwid: str) -> str:
    h = (hwid or "").strip().lower()
    if len(h) != 64 or any(c not in "0123456789abcdef" for c in h):
        raise LicenseError("bad_request", 400, "hwid phai la 64 ky tu hex (SHA-256)")
    return h


def _normalize_phone(phone: str) -> str:
    """Giu lai ky tu so va dau +, toi da 20 ky tu. Rong van chap nhan (user co the bo trong)."""
    s = "".join(ch for ch in (phone or "").strip() if ch.isdigit() or ch == "+")
    return s[:20]


# ---- Ma dung thu cong khai (vd "033.247.3333") -------------------------------------------------
def normalize_trial_code(text: str) -> Optional[str]:
    """'033.247.3333' / '033 247 3333' / '+84 33 247 3333' -> '0332473333'. Co chu cai -> None (khong phai ma so)."""
    if any(ch.isalpha() for ch in text):
        return None
    d = re.sub(r"\D", "", text)
    if d.startswith("84") and len(d) == 11:           # +84 33 247 3333 -> 0332473333
        d = "0" + d[2:]
    return d if 6 <= len(d) <= 15 else None


def _trial_code_hash(secret: bytes, digits: str) -> str:
    return hmac.new(secret, b"trialcode|" + digits.encode(), hashlib.sha256).hexdigest()


def add_trial_code(db, secret: bytes, text: str, days: int = PLANS["trial"], label: str = "",
                   now: Optional[int] = None):
    d = normalize_trial_code(text)
    if d is None:
        raise ValueError("ma dung thu phai la day so (6-15 chu so)")
    db.execute("INSERT OR REPLACE INTO trial_codes (code_hash,label,days,enabled,created_at) VALUES (?,?,?,1,?)",
               (_trial_code_hash(secret, d), label, days, _now(now)))


def set_trial_code_enabled(db, secret: bytes, text: str, enabled: bool):
    d = normalize_trial_code(text)
    if d is None or db.execute("UPDATE trial_codes SET enabled=? WHERE code_hash=?",
                               (1 if enabled else 0, _trial_code_hash(secret, d))).rowcount == 0:
        raise ValueError("khong co ma dung thu nay")


def _ip_hash(secret: bytes, ip: str) -> str:
    return hmac.new(secret, b"ip|" + ip.encode(), hashlib.sha256).hexdigest()


def _activate_by_trial_code(db, secret, signer, code_row, hwid, hw_label, app_version, client_ip, now) -> dict:
    """Trong giao dich cua activate(). Moi may (hwid) mot goi dung thu; nhap lai tren cung may -> cap lai token."""
    if not code_row["enabled"]:
        raise LicenseError("trial_closed", 403, "Da ngung cap dung thu, lien he nha cung cap")

    t = db.execute("SELECT license_id FROM trial_hwids WHERE hwid=?", (hwid,)).fetchone()
    if t is not None:                                          # may nay da nhan thu roi
        lic = db.execute("SELECT * FROM licenses WHERE id=?", (t["license_id"],)).fetchone()
        if lic["status"] == "revoked":
            raise LicenseError("revoked", 403, "Key da bi thu hoi")
        if lic["status"] != "active" or int(lic["expires_at"]) <= now:
            raise LicenseError("trial_used", 403, "May nay da dung thu truoc do")
        db.execute("UPDATE activations SET active=1,last_seen=?,hw_label=?,app_version=? WHERE license_id=? AND hwid=?",
                   (now, hw_label[:80], app_version[:32], lic["id"], hwid))
        return _issue(signer, lic, hwid, now)

    iph = _ip_hash(secret, client_ip) if client_ip else None
    if iph and MAX_TRIALS_PER_IP > 0:
        n = db.execute("SELECT COUNT(*) FROM trial_ips WHERE ip_hash=? AND used_at>?",
                       (iph, now - TRIAL_IP_WINDOW_DAYS * DAY)).fetchone()[0]
        if n >= MAX_TRIALS_PER_IP:
            raise LicenseError("trial_limit", 403, "Mang nay da nhan qua nhieu goi dung thu, lien he nha cung cap")

    days = int(code_row["days"])
    exp = now + days * DAY
    cur = db.execute(
        "INSERT INTO licenses (key_hash,key_hint,plan,duration_days,max_devices,resets_left,status,activated_at,"
        "expires_at,note,created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        (hmac.new(secret, b"autotrial|" + hwid.encode(), hashlib.sha256).hexdigest(), "AUTO", "trial", days, 1,
         DEFAULT_RESETS, "active", now, exp, "ma dung thu: " + (code_row["label"] or ""), now))
    lid = cur.lastrowid
    db.execute("INSERT INTO activations (license_id,hwid,hw_label,app_version,first_seen,last_seen) VALUES (?,?,?,?,?,?)",
               (lid, hwid, hw_label[:80], app_version[:32], now, now))
    db.execute("INSERT INTO trial_hwids (hwid,license_id,used_at) VALUES (?,?,?)", (hwid, lid, now))
    if iph:
        db.execute("INSERT INTO trial_ips (ip_hash,used_at) VALUES (?,?)", (iph, now))
    lic = db.execute("SELECT * FROM licenses WHERE id=?", (lid,)).fetchone()
    return _issue(signer, lic, hwid, now)


# ---- Quan tri ----------------------------------------------------------------------------------
def create_licenses(db, secret: bytes, plan: str = "m1", count: int = 1, days: Optional[int] = None,
                    until: Optional[int] = None, devices: int = DEFAULT_DEVICES,
                    activate_within_days: Optional[int] = None, note: str = "",
                    now: Optional[int] = None) -> list[str]:
    now = _now(now)
    if plan in PLANS:
        if days is not None or until is not None:
            raise ValueError("Goi co san khong dung --days/--until; dung plan 'custom'")
        days = PLANS[plan]
    elif plan == "custom":
        if (days is None) == (until is None):
            raise ValueError("plan custom can DUNG MOT trong hai: days hoac until")
        if days is not None and days <= 0:
            raise ValueError("days phai > 0")
        if until is not None and until <= now:
            raise ValueError("until phai nam trong tuong lai")
    else:
        raise ValueError("plan khong hop le: " + plan)

    keys = []
    for _ in range(count):
        key = gen_key()
        db.execute(
            "INSERT INTO licenses (key_hash,key_hint,plan,duration_days,fixed_expires_at,activate_before,"
            "max_devices,resets_left,note,created_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
            (key_hash(secret, key), key[-4:], plan, None if until else days, until,
             now + activate_within_days * DAY if activate_within_days else None,
             devices, DEFAULT_RESETS, note, now))
        keys.append(key)
    return keys


def revoke(db, lic_id: int):
    if db.execute("UPDATE licenses SET status='revoked' WHERE id=?", (lic_id,)).rowcount == 0:
        raise ValueError("khong co license id " + str(lic_id))


def extend(db, lic_id: int, days: int, now: Optional[int] = None):
    """Gia han. Key da het han: cong tu BAY GIO (khong tru nguoc ve ngay het han cu)."""
    now = _now(now)
    lic = db.execute("SELECT * FROM licenses WHERE id=?", (lic_id,)).fetchone()
    if lic is None:
        raise ValueError("khong co license id " + str(lic_id))
    if lic["status"] == "unused":
        if lic["fixed_expires_at"]:
            db.execute("UPDATE licenses SET fixed_expires_at=fixed_expires_at+? WHERE id=?", (days * DAY, lic_id))
        else:
            db.execute("UPDATE licenses SET duration_days=duration_days+? WHERE id=?", (days, lic_id))
    else:
        db.execute("UPDATE licenses SET expires_at=?, status='active' WHERE id=?",
                   (max(int(lic["expires_at"]), now) + days * DAY, lic_id))


def reset_devices(db, lic_id: int):
    db.execute("UPDATE activations SET active=0 WHERE license_id=?", (lic_id,))
    db.execute("UPDATE licenses SET resets_left=? WHERE id=?", (DEFAULT_RESETS, lic_id))


def list_licenses(db):
    return db.execute(
        "SELECT l.*, (SELECT COUNT(*) FROM activations a WHERE a.license_id=l.id AND a.active=1) AS devices_used "
        "FROM licenses l ORDER BY l.id DESC").fetchall()


# ---- API cho client ----------------------------------------------------------------------------
def _issue(signer: Signer, lic, hwid: str, now: int) -> dict:
    exp = int(lic["expires_at"])
    payload = {"v": 1, "kid": int(lic["id"]), "hwid": hwid, "plan": lic["plan"],
               "issued_at": now, "expires_at": exp, "offline_until": min(exp, now + OFFLINE_DAYS * DAY)}
    return {"token": signer.sign(payload), "plan": lic["plan"], "expires_at": exp,
            "offline_until": payload["offline_until"], "server_time": now}


def _ensure_usable(lic, now: int):
    if lic["status"] == "revoked":
        raise LicenseError("revoked", 403, "Key da bi thu hoi")
    if lic["status"] == "active" and int(lic["expires_at"]) <= now:
        raise LicenseError("expired", 403, "Key da het han")


def activate(db, secret: bytes, signer: Signer, key: str, hwid: str, hw_label: str = "",
             app_version: str = "", now: Optional[int] = None, client_ip: str = "") -> dict:
    now = _now(now)
    hwid = _check_hwid(hwid)

    digits = normalize_trial_code(key or "")                  # nhap day so (vd 033.247.3333)? -> ma dung thu
    if digits is not None:
        with _tx(db):
            row = db.execute("SELECT * FROM trial_codes WHERE code_hash=?", (_trial_code_hash(secret, digits),)).fetchone()
            if row is None:
                raise LicenseError("invalid_key", 404, "Key khong ton tai")
            return _activate_by_trial_code(db, secret, signer, row, hwid, hw_label, app_version, client_ip, now)

    kh = key_hash(secret, key)
    with _tx(db):
        lic = db.execute("SELECT * FROM licenses WHERE key_hash=?", (kh,)).fetchone()
        if lic is None:
            raise LicenseError("invalid_key", 404, "Key khong ton tai")
        _ensure_usable(lic, now)

        if lic["status"] == "unused":                       # lan kich hoat dau tien: bat dau dem gio
            if lic["activate_before"] and now > int(lic["activate_before"]):
                raise LicenseError("activation_window_closed", 403, "Key da qua han kich hoat")
            exp = int(lic["fixed_expires_at"]) if lic["fixed_expires_at"] else now + int(lic["duration_days"]) * DAY
            if exp <= now:
                raise LicenseError("expired", 403, "Key da het han")
            db.execute("UPDATE licenses SET status='active', activated_at=?, expires_at=? WHERE id=?",
                       (now, exp, lic["id"]))
            lic = db.execute("SELECT * FROM licenses WHERE id=?", (lic["id"],)).fetchone()

        act = db.execute("SELECT * FROM activations WHERE license_id=? AND hwid=?", (lic["id"], hwid)).fetchone()
        if act is None or not act["active"]:
            used = db.execute("SELECT COUNT(*) FROM activations WHERE license_id=? AND active=1", (lic["id"],)).fetchone()[0]
            if used >= int(lic["max_devices"]):
                raise LicenseError("device_limit", 403, "Key da dung het so may cho phep")
            if lic["plan"] == "trial":
                t = db.execute("SELECT license_id FROM trial_hwids WHERE hwid=?", (hwid,)).fetchone()
                if t is not None and int(t["license_id"]) != int(lic["id"]):
                    raise LicenseError("trial_used", 403, "May nay da dung thu truoc do")
                db.execute("INSERT OR IGNORE INTO trial_hwids (hwid,license_id,used_at) VALUES (?,?,?)",
                           (hwid, lic["id"], now))
            if act is None:
                db.execute("INSERT INTO activations (license_id,hwid,hw_label,app_version,first_seen,last_seen) "
                           "VALUES (?,?,?,?,?,?)", (lic["id"], hwid, hw_label[:80], app_version[:32], now, now))
            else:
                db.execute("UPDATE activations SET active=1,last_seen=?,hw_label=?,app_version=? WHERE id=?",
                           (now, hw_label[:80], app_version[:32], act["id"]))
        else:
            db.execute("UPDATE activations SET last_seen=?,hw_label=?,app_version=? WHERE id=?",
                       (now, hw_label[:80] or act["hw_label"], app_version[:32] or act["app_version"], act["id"]))
        return _issue(signer, lic, hwid, now)


def _token_payload(signer: Signer, token: str) -> dict:
    p = verify_token(signer.pub, token or "")
    if not p or p.get("v") != 1:
        raise LicenseError("bad_token", 401, "Token khong hop le")
    return p


def refresh(db, signer: Signer, token: str, now: Optional[int] = None) -> dict:
    now = _now(now)
    p = _token_payload(signer, token)
    with _tx(db):
        lic = db.execute("SELECT * FROM licenses WHERE id=?", (p["kid"],)).fetchone()
        if lic is None:
            raise LicenseError("bad_token", 401, "Token khong hop le")
        _ensure_usable(lic, now)
        act = db.execute("SELECT * FROM activations WHERE license_id=? AND hwid=? AND active=1",
                         (lic["id"], p["hwid"])).fetchone()
        if act is None:
            raise LicenseError("device_removed", 403, "May nay da bi go khoi key")
        db.execute("UPDATE activations SET last_seen=? WHERE id=?", (now, act["id"]))
        return _issue(signer, lic, p["hwid"], now)


def deactivate(db, signer: Signer, token: str, now: Optional[int] = None) -> dict:
    """Nguoi dung tu go may nay de chuyen sang may khac (tru vao resets_left)."""
    now = _now(now)
    p = _token_payload(signer, token)
    with _tx(db):
        lic = db.execute("SELECT * FROM licenses WHERE id=?", (p["kid"],)).fetchone()
        if lic is None:
            raise LicenseError("bad_token", 401, "Token khong hop le")
        act = db.execute("SELECT * FROM activations WHERE license_id=? AND hwid=? AND active=1",
                         (lic["id"], p["hwid"])).fetchone()
        if act is None:
            return {"ok": True}
        if int(lic["resets_left"]) <= 0:
            raise LicenseError("no_resets_left", 403, "Het luot tu go may, lien he admin")
        db.execute("UPDATE activations SET active=0 WHERE id=?", (act["id"],))
        db.execute("UPDATE licenses SET resets_left=resets_left-1 WHERE id=?", (lic["id"],))
        return {"ok": True, "resets_left": int(lic["resets_left"]) - 1}


# ---- Quy tac phia client (de viet lai bang C++; dung de kiem thu) ------------------------------
def local_state(payload: dict, my_hwid: str, now: int, last_seen: int) -> str:
    """
    ok          : cho chay
    need_online : phai noi server de xac thuc lai (qua han offline, hoac dong ho bi lui)
    expired     : het han
    invalid     : token khong thuoc may nay / sai
    Client da kiem tra chu ky TRUOC khi goi ham nay.
    """
    if payload.get("hwid") != my_hwid:
        return "invalid"
    if now >= payload["expires_at"]:
        return "expired"
    if now < last_seen - CLOCK_SLACK:
        return "need_online"
    if now >= payload["offline_until"]:
        return "need_online"
    return "ok"


# ---- CLI quan tri ------------------------------------------------------------------------------
def _fmt(ts) -> str:
    return "-" if not ts else datetime.fromtimestamp(int(ts), timezone.utc).strftime("%Y-%m-%d %H:%M")


def _data_dir() -> Path:
    d = Path(os.environ.get("PTLICENSE_DIR", "./data"))
    d.mkdir(parents=True, exist_ok=True)
    return d


def load_state():
    """Tra ve (db_path, secret, signer). Tao file neu chua co."""
    d = _data_dir()
    sf = d / "secret.key"
    if not sf.exists():
        sf.write_text(secrets.token_hex(32))
        try:
            os.chmod(sf, 0o600)
        except OSError:
            pass
    secret = bytes.fromhex(sf.read_text().strip())
    signer = Signer.load_or_create(d / "ed25519_private.pem")
    return d / "licenses.db", secret, signer


# ---- Claim flow: khach bam "Kich hoat" (de trong key) -> admin duyet tren web --------
def _device_row(db, hwid: str):
    return db.execute("SELECT * FROM devices WHERE hwid=?", (hwid,)).fetchone()


def _throttle(db, kind: str, hwid: str, ip: str, now: int, min_gap: int = 0, per_hour: int = 0,
              per_day: int = 0, ip_per_hour: int = 0) -> int:
    """Chong spam. Goi BEN TRONG _tx. Tra ve so giay phai doi (0 = duoc phep, va lan goi nay duoc ghi lai).
    Lan bi chan KHONG duoc ghi them, nen bam lien tuc khong keo dai thoi gian cho."""
    db.execute("DELETE FROM request_log WHERE ts < ?", (now - DAY,))
    mine = [r["ts"] for r in db.execute(
        "SELECT ts FROM request_log WHERE kind=? AND hwid=? ORDER BY ts", (kind, hwid))]
    wait = 0
    if mine and min_gap:
        wait = max(wait, mine[-1] + min_gap - now)
    hour = [t for t in mine if now - t < 3600]
    if per_hour and len(hour) >= per_hour:
        wait = max(wait, hour[-per_hour] + 3600 - now)
    if per_day and len(mine) >= per_day:
        wait = max(wait, mine[-per_day] + DAY - now)
    if ip and ip_per_hour:
        ips = [r["ts"] for r in db.execute(
            "SELECT ts FROM request_log WHERE kind=? AND ip=? AND ts>? ORDER BY ts", (kind, ip, now - 3600))]
        if len(ips) >= ip_per_hour:
            wait = max(wait, ips[-ip_per_hour] + 3600 - now)
    if wait > 0:
        return int(wait)
    db.execute("INSERT INTO request_log (kind,hwid,ip,ts) VALUES (?,?,?,?)", (kind, hwid, ip or "", now))
    return 0


def _limited(wait: int) -> "LicenseError":
    return LicenseError("rate_limited", 429, "Gui qua nhieu lan, thu lai sau %d giay" % wait, retry_after=wait)


def claim_device(db, signer: Signer, hwid: str, hw_label: str = "", app_version: str = "",
                 client_ip: str = "", now: Optional[int] = None) -> dict:
    """
    Khach bam Kich hoat (de trong key). Neu may da duoc duyet -> tra token.
    Neu may la/chua duyet -> ghi vao danh sach CHO DUYET, tra not_approved.
    """
    now = _now(now)
    hwid = _check_hwid(hwid)
    hw_label = (hw_label or "")[:80]
    app_version = (app_version or "")[:20]

    # NOTE: LicenseError raised INSIDE _tx would ROLLBACK the write, so decide first, raise after COMMIT.
    problem = None
    result = None
    with _tx(db):
        wait = _throttle(db, "claim", hwid, client_ip, now, CLAIM_MIN_GAP, CLAIM_MAX_PER_HOUR, 0, CLAIM_IP_MAX_PER_HOUR)
        if wait:
            raise _limited(wait)       # chua ghi gi ngoai nhat ky -> an toan khi ROLLBACK
        row = _device_row(db, hwid)
        if row is None and _throttle(db, "newdev", hwid, client_ip, now, 0, 0, 0, NEWDEV_IP_MAX_PER_HOUR):
            row = False                # IP nay tao qua nhieu may moi: khong ghi them
        if row is False:
            problem = ("rate_limited", 429, "Qua nhieu may moi tu mang nay, thu lai sau")
        elif row is None:
            # may lan dau -> ghi vao cho duyet
            db.execute(
                "INSERT INTO devices (hwid,hw_label,status,first_seen,last_seen,last_ip,app_version) "
                "VALUES (?,?,?,?,?,?,?)",
                (hwid, hw_label, "pending", now, now, client_ip or "", app_version))
            problem = ("not_approved", 403, "May nay chua duoc kich hoat, vui long gui Ma may cho nha cung cap")
        else:
            # cap nhat last_seen/label
            db.execute("UPDATE devices SET last_seen=?,last_ip=?,hw_label=?,app_version=? WHERE hwid=?",
                       (now, client_ip or row["last_ip"] or "", hw_label or row["hw_label"] or "",
                        app_version or row["app_version"] or "", hwid))
            row = _device_row(db, hwid)
            if row["status"] == "revoked":
                problem = ("revoked", 403, "May nay da bi thu hoi")
            elif row["status"] != "approved":
                problem = ("not_approved", 403, "May nay chua duoc kich hoat, vui long gui Ma may cho nha cung cap")
            elif not row["expires_at"] or int(row["expires_at"]) <= now:
                problem = ("expired", 403, "Key da het han")
            else:
                payload = {"v": 1, "kid": 0, "hwid": hwid, "plan": row["plan"] or "custom",
                           "issued_at": now, "expires_at": int(row["expires_at"]),
                           "offline_until": min(int(row["expires_at"]), now + OFFLINE_DAYS * DAY)}
                result = {"token": signer.sign(payload), "plan": payload["plan"],
                          "expires_at": payload["expires_at"], "offline_until": payload["offline_until"],
                          "server_time": now}

    if problem:
        raise LicenseError(*problem)
    return result


def request_device(db, hwid: str, phone: str = "", hw_label: str = "", app_version: str = "",
                   client_ip: str = "", now: Optional[int] = None) -> dict:
    """
    User bam 'Gui yeu cau' tu dialog (thay vi nhap key). Ghi may vao danh sach CHO DUYET
    kem so dien thoai de admin lien he.Neu da co thi chi cap nhat SĐT + last_seen.
    """
    now = _now(now)
    hwid = _check_hwid(hwid)
    hw_label = (hw_label or "")[:80]
    app_version = (app_version or "")[:20]
    phone = _normalize_phone(phone)
    final_status = "pending"
    wait = 0

    with _tx(db):
        wait = _throttle(db, "request", hwid, client_ip, now, REQ_MIN_GAP, REQ_MAX_PER_HOUR, REQ_MAX_PER_DAY,
                         REQ_IP_MAX_PER_HOUR)
        row = _device_row(db, hwid) if not wait else None
        if wait:
            pass                                   # bi chan: ghi nhat ky xong, khong dong vao bang devices
        elif row is None:
            wait = _throttle(db, "newdev", hwid, client_ip, now, 0, 0, 0, NEWDEV_IP_MAX_PER_HOUR)
            if not wait:
                db.execute(
                    "INSERT INTO devices (hwid,hw_label,status,first_seen,last_seen,last_ip,app_version,phone,requested_at) "
                    "VALUES (?,?,?,?,?,?,?,?,?)",
                    (hwid, hw_label, "pending", now, now, client_ip or "", app_version, phone, now))
        else:
            # Giu nguyen trang thai: may dang duoc duyet (con han) hoac bi thu hoi KHONG bi ha ve 'pending'
            # (neu ha, claim_refresh se bao device_removed va user mat quyen dang dung).
            st = row["status"]
            if st == "approved" and (not row["expires_at"] or int(row["expires_at"]) <= now):
                st = "pending"                               # het han -> dua lai vao cho duyet de gia han
            elif st not in ("approved", "revoked"):
                st = "pending"
            db.execute("UPDATE devices SET status=?,last_seen=?,last_ip=?,hw_label=?,"
                       "app_version=?,phone=?,requested_at=? WHERE hwid=?",
                       (st, now, client_ip or row["last_ip"] or "", hw_label or row["hw_label"] or "",
                        app_version or row["app_version"] or "", phone or row["phone"] or "", now, hwid))
            final_status = st
    if wait:
        raise _limited(wait)                       # NGOAI _tx: nhat ky chan van duoc COMMIT
    return {"ok": True, "hwid": hwid, "status": final_status}


def claim_refresh(db, signer: Signer, token: str, now: Optional[int] = None) -> dict:
    """Refresh cho token cua claim flow (kid=0). Doc han tu bang devices, khong phai tu token."""
    now = _now(now)
    p = _token_payload(signer, token)
    with _tx(db):
        row = _device_row(db, p["hwid"])
        if row is None:
            raise LicenseError("bad_token", 401, "Token khong hop le")
        if row["status"] == "revoked":
            raise LicenseError("revoked", 403, "May nay da bi thu hoi")
        if row["status"] != "approved":
            raise LicenseError("device_removed", 403, "May nay da bi go khoi key")
        if not row["expires_at"] or int(row["expires_at"]) <= now:
            raise LicenseError("expired", 403, "Key da het han")
        db.execute("UPDATE devices SET last_seen=? WHERE hwid=?", (now, p["hwid"]))
        payload = {"v": 1, "kid": 0, "hwid": p["hwid"], "plan": row["plan"] or "custom",
                   "issued_at": now, "expires_at": int(row["expires_at"]),
                   "offline_until": min(int(row["expires_at"]), now + OFFLINE_DAYS * DAY)}
        return {"token": signer.sign(payload), "plan": payload["plan"],
                "expires_at": payload["expires_at"], "offline_until": payload["offline_until"],
                "server_time": now}


# ---- Admin: duyet / gia han / thu hoi thiet bi --------------------------------------
def approve_device(db, hwid: str, plan: str = "m1", days: Optional[int] = None,
                   until: Optional[int] = None, note: str = "", now: Optional[int] = None) -> dict:
    """Duyet 1 may: dat status=approved, plan, expires_at."""
    now = _now(now)
    hwid = _check_hwid(hwid)
    if plan in PLANS:
        exp = now + PLANS[plan] * DAY
    elif plan == "custom":
        if days is not None:
            exp = now + int(days) * DAY
        elif until is not None:
            exp = int(until)
        else:
            raise ValueError("plan custom can days hoac until")
    else:
        raise ValueError("plan khong hop le: " + plan)

    with _tx(db):
        row = _device_row(db, hwid)
        if row is None:
            db.execute("INSERT INTO devices (hwid,status,plan,approved_at,expires_at,first_seen,last_seen,note) "
                       "VALUES (?,?,?,?,?,?,?,?)", (hwid, "approved", plan, now, exp, now, now, note))
        else:
            db.execute("UPDATE devices SET status=?,plan=?,approved_at=?,expires_at=?,note=?,requested_at=NULL "
                       "WHERE hwid=?",
                       ("approved", plan, now, exp, note or row["note"] or "", hwid))
    return {"ok": True, "hwid": hwid, "plan": plan, "expires_at": exp}


def extend_device(db, hwid: str, days: int, now: Optional[int] = None) -> dict:
    """Gia han: con han -> cong noi; het han -> tinh tu hom nay."""
    now = _now(now)
    hwid = _check_hwid(hwid)
    with _tx(db):
        row = _device_row(db, hwid)
        if row is None:
            raise ValueError("khong co thiet bi " + hwid)
        base = max(int(row["expires_at"] or 0), now)
        exp = base + int(days) * DAY
        db.execute("UPDATE devices SET expires_at=?,status=? WHERE hwid=?",
                   (exp, "approved" if row["status"] != "revoked" else "revoked", hwid))
    return {"ok": True, "hwid": hwid, "expires_at": exp}


def revoke_device(db, hwid: str) -> dict:
    hwid = _check_hwid(hwid)
    with _tx(db):
        if db.execute("UPDATE devices SET status='revoked' WHERE hwid=?", (hwid,)).rowcount == 0:
            raise ValueError("khong co thiet bi " + hwid)
    return {"ok": True, "hwid": hwid}


def list_devices(db, limit: int = 200):
    return db.execute("SELECT * FROM devices ORDER BY last_seen DESC LIMIT ?", (limit,)).fetchall()


def list_pending(db, limit: int = 200):
    return db.execute("SELECT * FROM devices WHERE status='pending' ORDER BY first_seen DESC LIMIT ?",
                      (limit,)).fetchall()




def main(argv=None):
    ap = argparse.ArgumentParser(prog="ptkey")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("init", help="tao secret + cap khoa ky + database, in public key de dan vao client")
    c = sub.add_parser("create", help="tao key")
    c.add_argument("--plan", default="m1", choices=list(PLANS) + ["custom"])
    c.add_argument("--count", type=int, default=1)
    c.add_argument("--days", type=int)
    c.add_argument("--until", help="YYYY-MM-DD (UTC) - ngay het han co dinh")
    c.add_argument("--devices", type=int, default=DEFAULT_DEVICES)
    c.add_argument("--activate-within", type=int, help="so ngay phai kich hoat ke tu luc tao")
    c.add_argument("--note", default="")
    sub.add_parser("list")
    t = sub.add_parser("trialcode", help="ma dung thu cong khai (vd so dien thoai) de khach tu nhan 3 ngay")
    t.add_argument("action", choices=["add", "disable", "enable", "list"])
    t.add_argument("code", nargs="?", default="")
    t.add_argument("--days", type=int, default=PLANS["trial"])
    t.add_argument("--label", default="")
    for name in ("revoke", "reset"):
        sub.add_parser(name).add_argument("id", type=int)
    e = sub.add_parser("extend")
    e.add_argument("id", type=int)
    e.add_argument("days", type=int)
    a = ap.parse_args(argv)

    db_path, secret, signer = load_state()
    db = connect(db_path)

    if a.cmd == "init":
        print("Public key (base64, 32 byte) - dan vao client:\n  " + signer.public_b64())
    elif a.cmd == "create":
        until = None
        if a.until:
            until = int(datetime.strptime(a.until, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp())
        plan = "custom" if (a.days or until) and a.plan == "m1" else a.plan
        for k in create_licenses(db, secret, plan, a.count, a.days, until, a.devices, a.activate_within, a.note):
            print(k)
    elif a.cmd == "list":
        print(f"{'ID':>4} {'HINT':<5} {'PLAN':<7} {'STATUS':<8} {'DEV':<5} {'ACTIVATED':<17} {'EXPIRES':<17} NOTE")
        for r in list_licenses(db):
            print(f"{r['id']:>4} {r['key_hint']:<5} {r['plan']:<7} {r['status']:<8} "
                  f"{r['devices_used']}/{r['max_devices']:<3} {_fmt(r['activated_at']):<17} {_fmt(r['expires_at']):<17} {r['note']}")
    elif a.cmd == "trialcode":
        if a.action == "add":
            add_trial_code(db, secret, a.code, a.days, a.label)
        elif a.action in ("disable", "enable"):
            set_trial_code_enabled(db, secret, a.code, a.action == "enable")
        else:
            for r in db.execute("SELECT * FROM trial_codes"):
                print(f"{r['code_hash'][:8]}…  {r['days']} ngay  {'BAT' if r['enabled'] else 'TAT'}  {r['label']}")
    elif a.cmd == "revoke":
        revoke(db, a.id)
    elif a.cmd == "reset":
        reset_devices(db, a.id)
    elif a.cmd == "extend":
        extend(db, a.id, a.days)


if __name__ == "__main__":
    main()
