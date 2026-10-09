#!/usr/bin/env python3
"""
Vá tone_cache_server.py: tách API key theo route + thêm auth cho /api/worker/job & /result.

Vì sao là script mà không phải file .patch: file thật nằm trên VPS (không có trong vps.zip) nên không thể
sinh diff chính xác. Script tự định vị bằng AST, tự kiểm tra, và DỪNG nếu cấu trúc khác dự kiến.

Cách vá (chỉ 2 chỗ, không đụng vào từng endpoint):
  1. Chèn middleware "route scope" ngay sau dòng `app = FastAPI(...)`  -> kiểm key theo đường dẫn.
  2. Thay thân hàm `_check_auth` để chấp nhận mọi key hợp lệ (middleware đã phân quyền theo route,
     nên các endpoint cũ vẫn gọi _check_auth bình thường mà không bị từ chối key mới).

Dùng:
  python apply_tone_cache_split_keys.py /opt/tone-cache-server/tone_cache_server.py            # ghi ra .patched
  python apply_tone_cache_split_keys.py /opt/tone-cache-server/tone_cache_server.py --in-place # có .bak-secfix
"""
import ast, re, shutil, sys

MARK = "# === SECURITY PATCH split-keys"

MIDDLEWARE = '''
# === SECURITY PATCH split-keys: BEGIN ===========================================================
import hmac as _sec_hmac
import os as _sec_os
from fastapi.responses import JSONResponse as _SecJSON

_SEC_PTLIVE_KEY = _sec_os.environ.get("PTLIVE_API_KEY", "").strip()
_SEC_SYNC_KEY   = _sec_os.environ.get("SYNC_API_KEY", "").strip()
# 1 = key cu (TONE_CACHE_API_KEY, dang phat cho moi khach) van duoc /sync + /all (giai doan chuyen tiep, cho ptStudio)
# 0 = chi SYNC_API_KEY moi duoc /sync + /all  <- dat 0 khi ptStudio da chuyen sang sync key
_SEC_LEGACY_SYNC = (_sec_os.environ.get("LEGACY_KEY_ALLOW_SYNC", "1").split() or ["1"])[0] != "0"   # chi lay token dau (phong comment lot vao)

_SEC_SYNC_EXACT   = ("/api/tone-cache/all", "/api/tone-cache/sync")
_SEC_PTLIVE_EXACT = ("/api/worker/job", "/api/workers/state", "/api/crawler/status")
_SEC_PTLIVE_PREFIX = ("/api/tone-cache/check/", "/api/worker/result/", "/api/audio/")


def _sec_match(key: str, candidates) -> bool:
    kb = key.encode("utf-8")
    ok = False
    for c in candidates:
        if c and _sec_hmac.compare_digest(kb, c.encode("utf-8")):   # bo qua key rong -> khong bao gio khop ""
            ok = True
    return ok


def _sec_keys_ptlive():
    return (_SEC_PTLIVE_KEY, _SEC_SYNC_KEY, API_KEY)               # API_KEY = key cu, giu tuong thich


def _sec_keys_sync():
    return (_SEC_SYNC_KEY, API_KEY) if _SEC_LEGACY_SYNC else (_SEC_SYNC_KEY,)


@app.middleware("http")
async def _sec_route_scope(request, call_next):
    if request.method == "OPTIONS":
        return await call_next(request)
    path = request.url.path
    key = request.headers.get("x-api-key", "")
    if path in _SEC_SYNC_EXACT:
        if not _sec_match(key, _sec_keys_sync()):
            # key hop le cho ptLive nhung khong du quyen sync/all -> 403, con lai -> 401
            code = 403 if (key and _sec_match(key, _sec_keys_ptlive())) else 401
            return _SecJSON({"detail": "Sync key required" if code == 403 else "Invalid API key"}, status_code=code)
    elif path in _SEC_PTLIVE_EXACT or path.startswith(_SEC_PTLIVE_PREFIX):
        if not _sec_match(key, _sec_keys_ptlive()):
            return _SecJSON({"detail": "Invalid API key"}, status_code=401)
    return await call_next(request)


def _sec_any_valid_key(x_api_key):
    return bool(x_api_key) and _sec_match(x_api_key, _sec_keys_ptlive())
# === SECURITY PATCH split-keys: END =============================================================
'''

NEW_CHECK_AUTH = '''def _check_auth(x_api_key: str | None = None):
    # PATCHED: phan quyen theo route nam o middleware _sec_route_scope; o day chi can key hop le.
    if not _sec_any_valid_key(x_api_key):
        raise HTTPException(status_code=401, detail="Invalid API key")
'''


def die(msg):
    print("LOI: " + msg, file=sys.stderr)
    sys.exit(2)


def main():
    if len(sys.argv) < 2:
        die(__doc__)
    path = sys.argv[1]
    in_place = "--in-place" in sys.argv
    src = open(path, encoding="utf-8").read()
    if MARK in src:
        print("Da vá rồi (có marker) - bỏ qua.")
        return
    tree = ast.parse(src)
    lines = src.splitlines(keepends=True)

    # --- tìm app = FastAPI(...) ---
    app_end = None
    for n in tree.body:
        if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "app" for t in n.targets) \
           and isinstance(n.value, ast.Call) and getattr(n.value.func, "id", getattr(n.value.func, "attr", "")) == "FastAPI":
            app_end = n.end_lineno
    if app_end is None:
        die("Không thấy dòng `app = FastAPI(...)` ở top-level.")

    # --- API_KEY phải tồn tại ở top-level ---
    if not any(isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "API_KEY" for t in n.targets)
               for n in tree.body):
        die("Không thấy biến top-level API_KEY (middleware cần nó cho key cũ).")

    # --- tìm _check_auth ---
    fn = next((n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == "_check_auth"), None)
    if fn is None:
        die("Không thấy hàm _check_auth.")
    if isinstance(fn, ast.AsyncFunctionDef):
        die("_check_auth là async - cần vá tay.")
    if fn.decorator_list:
        die("_check_auth có decorator - cần vá tay.")
    if fn.lineno <= app_end:
        die("_check_auth nằm trước `app = FastAPI` - cần vá tay.")

    # --- áp dụng sửa từ dưới lên ---
    out = lines[:]
    out[fn.lineno - 1:fn.end_lineno] = [NEW_CHECK_AUTH]
    out[app_end:app_end] = [MIDDLEWARE]
    new_src = "".join(out)

    try:
        compile(new_src, path, "exec")
    except SyntaxError as e:
        die(f"File sau khi vá bị lỗi cú pháp: {e}")
    if "HTTPException" not in src:
        die("File gốc không import HTTPException - kiểm tra lại.")

    if in_place:
        shutil.copy2(path, path + ".bak-secfix")
        target = path
    else:
        target = path + ".patched"
    open(target, "w", encoding="utf-8").write(new_src)
    print(f"OK -> {target}")
    print("Env cần thêm vào systemd: PTLIVE_API_KEY, SYNC_API_KEY (và LEGACY_KEY_ALLOW_SYNC=1 trong giai đoạn chuyển tiếp)")


if __name__ == "__main__":
    main()
