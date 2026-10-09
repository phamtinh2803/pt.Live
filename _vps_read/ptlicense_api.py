"""
api.py - lop HTTP mong cho ptlicense (FastAPI). Chay:  uvicorn api:app --host 127.0.0.1 --port 8765
(da kiem thu phan loi ptlicense.py; file nay la khung ghep, hay thu tren VPS cua ban truoc khi phat hanh)
"""
import hmac
import os
from pathlib import Path
import ptlicense as L
from fastapi import FastAPI, Request, Query
from fastapi.responses import JSONResponse, HTMLResponse

DB_PATH, SECRET, SIGNER = L.load_state()
L.connect(DB_PATH).close()                      # tao bang neu chua co
app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)   # tat trang docs tren production

# Token admin cho trang web / API quan tri (doc tu env de khong hardcode)
ADMIN_TOKEN = os.environ.get("PTLICENSE_ADMIN_TOKEN", "")


def _err(code: str, http: int, message: str = "", retry_after: int = 0):
    body = {"error": code, "message": message or code}
    headers = None
    if retry_after > 0:                                    # rate_limited: app dung de khoa nut dung so giay
        body["retry_after"] = int(retry_after)
        headers = {"Retry-After": str(int(retry_after))}
    return JSONResponse(body, status_code=http, headers=headers)


@app.exception_handler(L.LicenseError)
async def _license_error(_: Request, e: L.LicenseError):
    return _err(e.code, e.http, e.message, getattr(e, "retry_after", 0))


@app.exception_handler(ValueError)
async def _value_error(_: Request, e: ValueError):
    return _err("bad_request", 400, str(e))


def _body(d) -> dict:
    if not isinstance(d, dict):
        raise L.LicenseError("bad_request", 400, "JSON object expected")
    return d


def _check_admin(request: Request):
    if not ADMIN_TOKEN:
        raise L.LicenseError("bad_request", 400, "server chua cau hinh admin token")
    # Chi nhan qua header (query string bi ghi vao access log / history / Referer). So sanh hang so thoi gian.
    tok = request.headers.get("x-admin-token", "")
    if not tok or not hmac.compare_digest(tok.encode("utf-8"), ADMIN_TOKEN.encode("utf-8")):
        raise L.LicenseError("unauthorized", 401, "sai admin token")


@app.post("/api/license/activate")
def activate(body: dict, request: Request):
    b = _body(body)
    ip = request.headers.get("x-real-ip") or (request.client.host if request.client else "")
    db = L.connect(DB_PATH)
    try:
        return L.activate(db, SECRET, SIGNER, str(b.get("key", "")), str(b.get("hwid", "")),
                          str(b.get("hw_label", "")), str(b.get("app_version", "")), client_ip=ip)
    finally:
        db.close()


@app.post("/api/license/claim")
def claim(body: dict, request: Request):
    """Khach bam Kich hoat (de trong key). Neu duyet -> tra token; chua duyet -> 403 not_approved."""
    b = _body(body)
    ip = request.headers.get("x-real-ip") or (request.client.host if request.client else "")
    db = L.connect(DB_PATH)
    try:
        return L.claim_device(db, SIGNER, str(b.get("hwid", "")), str(b.get("hw_label", "")),
                              str(b.get("app_version", "")), client_ip=ip)
    finally:
        db.close()


@app.post("/api/license/request")
def request_activation(body: dict, request: Request):
    """User bam 'Gui yeu cau' tu dialog: gui hwid + SĐT, server ghi vao danh sach cho duyet."""
    b = _body(body)
    ip = request.headers.get("x-real-ip") or (request.client.host if request.client else "")
    db = L.connect(DB_PATH)
    try:
        return L.request_device(db, str(b.get("hwid", "")), str(b.get("phone", "")),
                                str(b.get("hw_label", "")), str(b.get("app_version", "")), client_ip=ip)
    finally:
        db.close()


@app.post("/api/license/refresh")
def refresh(body: dict, request: Request):
    b = _body(body)
    token = str(b.get("token", ""))
    db = L.connect(DB_PATH)
    try:
        # Phan biet token claim (kid=0) va token key (kid>0)
        p = L.verify_token(SIGNER.pub, token)
        if p and p.get("kid") == 0:
            return L.claim_refresh(db, SIGNER, token)
        return L.refresh(db, SIGNER, token)
    finally:
        db.close()


@app.post("/api/license/deactivate")
def deactivate(body: dict):
    b = _body(body)
    db = L.connect(DB_PATH)
    try:
        return L.deactivate(db, SIGNER, str(b.get("token", "")))
    finally:
        db.close()


@app.get("/api/license/time")
def server_time():
    import time
    return {"server_time": int(time.time())}


# ---- Admin endpoints -----------------------------------------------------------------------
@app.post("/api/license/admin/approve")
def admin_approve(body: dict, request: Request):
    _check_admin(request)
    b = _body(body)
    db = L.connect(DB_PATH)
    try:
        hwid = str(b.get("hwid", "")).strip().lower()
        plan = str(b.get("plan", "m1"))
        days = b.get("days")
        until = b.get("until")
        note = str(b.get("note", ""))
        r = L.approve_device(db, hwid, plan, days, until, note)
        return r
    finally:
        db.close()


@app.post("/api/license/admin/extend")
def admin_extend(body: dict, request: Request):
    _check_admin(request)
    b = _body(body)
    db = L.connect(DB_PATH)
    try:
        hwid = str(b.get("hwid", "")).strip().lower()
        days = int(b.get("days", 30))
        return L.extend_device(db, hwid, days)
    finally:
        db.close()


@app.post("/api/license/admin/revoke")
def admin_revoke(body: dict, request: Request):
    _check_admin(request)
    b = _body(body)
    db = L.connect(DB_PATH)
    try:
        hwid = str(b.get("hwid", "")).strip().lower()
        return L.revoke_device(db, hwid)
    finally:
        db.close()


@app.get("/api/license/admin/list")
def admin_list(request: Request, status: str = Query(default=""), limit: int = Query(default=200)):
    _check_admin(request)
    db = L.connect(DB_PATH)
    try:
        if status == "pending":
            rows = L.list_pending(db, limit)
        else:
            rows = L.list_devices(db, limit)
        items = [dict(r) for r in rows]
        return {"items": items, "count": len(items)}
    finally:
        db.close()


@app.get("/admin.html", response_class=HTMLResponse)
async def admin_html():
    # admin.html lives alongside this api.py (e.g. /opt/ptlicense/admin.html)
    p = Path(__file__).resolve().parent / "admin.html"
    if not p.exists():
        return HTMLResponse("<h1>admin.html not found</h1>", status_code=404)
    return HTMLResponse(p.read_text(encoding="utf-8", errors="ignore"))
