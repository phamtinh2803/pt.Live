"""license_admin.py — License admin API factory (multi-brand).

Mỗi brand (pt, ddk) tạo 1 FastAPI app instance qua make_admin_app().
Env vars tương ứng:  {BRAND}_LICENSE_HMAC_SECRET, {BRAND}_LICENSE_ADMIN_TOKEN, ...
prefix="ddk" → env namespace = DDK_, prefix="" (hoặc "pt") dùng namespace PT_ cho tương thích cũ.
"""
import base64
import hashlib
import hmac
import os
import re
import sqlite3
from datetime import datetime, timedelta
from pathlib import Path

from fastapi import FastAPI, HTTPException, Header, Query
from fastapi.responses import HTMLResponse
from pydantic import BaseModel

# ── Constants ────────────────────────────────────────────────────────────────
VALID_DAYS = (7, 30, 182, 365, 0)
_MACHINE_RE = re.compile(r"^[0-9A-Fa-f]{32}$")


class LicenseRequest(BaseModel):
    machine_id: str
    days: int = 7
    note: str = ""


# ── Factory ──────────────────────────────────────────────────────────────────
def make_admin_app(
    *,
    prefix: str = "",
    title: str = "PT Studio",
    default_secret: str,
    default_html: str,
    default_db: str,
) -> FastAPI:
    """
    prefix  : env var namespace — "" hoặc "pt" cho PT, "DDK" cho DDK.
    prefix="" giữ tương thích env cũ (LICENSE_HMAC_SECRET, LICENSE_ADMIN_TOKEN).
    """
    ns = (prefix + "_") if prefix else ""

    secret = os.environ.get(f"{ns}LICENSE_HMAC_SECRET", default_secret).encode()
    admin_token = os.environ.get(f"{ns}LICENSE_ADMIN_TOKEN", "")
    db_path = Path(os.environ.get(f"{ns}LICENSE_DB", default_db))
    html_path = Path(os.environ.get(f"{ns}LICENSE_ADMIN_HTML", default_html))

    app = FastAPI(title=f"{title} License Admin", docs_url=None, redoc_url=None)

    # ── Helpers ──────────────────────────────────────────────────────────────
    def _init_db():
        with sqlite3.connect(db_path) as db:
            db.execute(
                """CREATE TABLE IF NOT EXISTS licenses (
                    id INTEGER PRIMARY KEY,
                    machine_id TEXT NOT NULL,
                    expiry TEXT NOT NULL,
                    note TEXT NOT NULL DEFAULT '',
                    license_key TEXT NOT NULL,
                    created_at TEXT NOT NULL
                )"""
            )
            db.commit()

    def _check_admin(token: str | None):
        if not admin_token or not hmac.compare_digest(token or "", admin_token):
            raise HTTPException(status_code=401, detail="Sai mat khau admin")

    def _lic_hmac(machine_id: str, expiry: str) -> str:
        msg = f"{machine_id}|{expiry}".encode("utf-8")
        return hmac.new(secret, msg, hashlib.sha256).hexdigest()

    def _gen_key(machine_id: str, expiry: str) -> str:
        mid = machine_id.strip().upper()
        digest = _lic_hmac(mid, expiry)
        payload = f"{mid}\n{expiry}\n{digest}"
        return base64.urlsafe_b64encode(payload.encode("utf-8")).decode("utf-8").rstrip("=")

    def _load_html() -> str:
        try:
            return html_path.read_text(encoding="utf-8")
        except Exception:
            return f"<h1>{title} License Admin — thieu admin html</h1>"

    # ── Routes ───────────────────────────────────────────────────────────────
    @app.get("/", response_class=HTMLResponse)
    @app.get("/admin", response_class=HTMLResponse)
    def admin_page():
        return _load_html()

    @app.post("/api/licenses")
    @app.post("/api/admin/licenses")
    def create_license(
        req: LicenseRequest,
        x_admin_token: str | None = Header(None),
    ):
        _check_admin(x_admin_token)
        mid = req.machine_id.strip().upper()
        if not _MACHINE_RE.fullmatch(mid):
            raise HTTPException(status_code=400, detail="Machine ID phai dung 32 ky tu hex")
        if req.days not in VALID_DAYS:
            raise HTTPException(status_code=400, detail="Thoi han khong hop le")
        expiry = "" if req.days == 0 else (datetime.now() + timedelta(days=req.days)).strftime("%Y-%m-%d")
        key = _gen_key(mid, expiry)
        note = req.note.strip()[:200]
        _init_db()
        with sqlite3.connect(db_path) as db:
            db.execute(
                "INSERT INTO licenses(machine_id, expiry, note, license_key, created_at) VALUES (?,?,?,?,?)",
                (mid, expiry, note, key, datetime.now().isoformat(timespec="seconds")),
            )
            db.commit()
        return {"license_key": key, "machine_id": mid, "expiry": expiry or None, "note": note}

    @app.get("/api/licenses")
    @app.get("/api/admin/licenses")
    def list_licenses(
        q: str = Query("", max_length=64),
        x_admin_token: str | None = Header(None),
    ):
        _check_admin(x_admin_token)
        _init_db()
        like = f"%{q.strip()}%"
        with sqlite3.connect(db_path) as db:
            db.row_factory = sqlite3.Row
            rows = db.execute(
                "SELECT machine_id, expiry, note, license_key, created_at FROM licenses "
                "WHERE machine_id LIKE ? OR note LIKE ? ORDER BY id DESC LIMIT 200",
                (like, like),
            ).fetchall()
        items = []
        for row in rows:
            item = dict(row)
            item["expiry"] = item["expiry"] or None
            items.append(item)
        return {"items": items}

    return app
