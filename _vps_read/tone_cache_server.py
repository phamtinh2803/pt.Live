"""
tone_cache_server.py
--------------------
FastAPI server chia se tone_cache.json cho nhieu user.
Merge rule: deep > standard (deep de len standard, standard KHONG de len deep).
"""

from __future__ import annotations

import json
import asyncio
import logging
import os
import sys
import threading
import time
from pathlib import Path

LOG = logging.getLogger("tone-cache")

# --- Logging: đảm bảo LOG.info xuat ra journal (systemd) ---
logging.basicConfig(
    format='%(asctime)s %(levelname)s: %(message)s',
    level=logging.INFO,
    handlers=[logging.StreamHandler()],
    force=True,
)


from fastapi import FastAPI, Header, HTTPException, Query, Body
import importlib
try:
    from fastapi import WebSocket
except ImportError:
    WebSocket = None  # type: ignore
try:
    from starlette.websockets import WebSocketState as _WSS
except ImportError:
    _WSS = None
from fastapi.responses import FileResponse
from fastapi import BackgroundTasks
import re
import subprocess
import tempfile
import shutil
import shutil
from pydantic import BaseModel

# ── Config ────────────────────────────────────────────────────────────────────
API_KEY = os.environ.get("TONE_CACHE_API_KEY", "ptstudio-tone-cache-2025")
WORKER_WS_SECRET = os.environ.get("WORKER_WS_SECRET", "da6e1ce321fdee080b1ba5c06e358e90-worker")
# Comma-separated tokens; legacy WORKER_WS_SECRET remains accepted.
_WORKER_WS_TOKENS = {token.strip() for token in os.environ.get("WORKER_WS_TOKENS", WORKER_WS_SECRET).split(",") if token.strip()}
_WORKER_NAS_TOKEN = os.environ.get("WORKER_NAS_TOKEN", "")

# ── Idle crawler (tự tăng data khi rảnh) ─────────────────────────────────────
_CRAWLER_ENABLED = os.environ.get("CRAWLER_ENABLED", "false").strip().lower() in ("1", "true", "yes", "on")
_crawler = None  # khởi tạo sau khi CACHE_FILE + dispatcher sẵn sàng


def _worker_kind(token: str | None) -> str:
    return "nas" if token and token == _WORKER_NAS_TOKEN else "pc"


def _valid_worker_token(token: str | None) -> bool:
    return bool(token) and token in _WORKER_WS_TOKENS

CACHE_FILE = Path(os.environ.get("TONE_CACHE_FILE", "/opt/tone-cache-server/tone_cache.json"))
MAX_ENTRIES = None  # gioi han cache tren server

# ── Import idle_crawler (sau CACHE_FILE đã biết) ───────────────────────────
try:
    sys.path.insert(0, str(Path(__file__).parent))
    from idle_crawler import CrawlerConfig, CrawlState, IdleScheduler, _normalize_artist_name as _norm_artist, _is_allowed_title as _crawl_is_allowed
except Exception as exc:
    LOG.warning("idle_crawler không load được (chưa cần thiết): %s", exc)
    _crawl_available = False
else:
    _crawl_available = True

app = FastAPI(title="PT Studio Tone Cache Server")

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
_CRAWLER_JOB_URL = "http://127.0.0.1:8000/api/worker/job"

# ── Khởi tạo idle crawler (nếu enabled) ────────────────────────────────────
if _crawl_available and _CRAWLER_ENABLED:
    try:
        _config = CrawlerConfig(CACHE_FILE.parent / "crawler_config.json")
        _state = CrawlState(CACHE_FILE.parent / "crawl_state.json")

        import urllib.request as _ur

        def _dispatch_crawl_job(vid: str, u: str, t: str) -> bool:
            """Dispatch job cho crawler — gọi loopback HTTP vào chính broker."""
            try:
                jdata = json.dumps({"video_id": vid, "url": u, "title": t,
                                    "mode": "standard", "source": "crawler"}).encode("utf-8")
                req = _ur.Request(_CRAWLER_JOB_URL, data=jdata,
                                  headers={"Content-Type": "application/json", "X-API-Key": API_KEY})
                with _ur.urlopen(req, timeout=10) as resp:
                    res = json.loads(resp.read().decode())
                    ok = res.get("status") == "queued"
                    LOG.debug("Dispatch crawl %s: %s", vid, "OK" if ok else "FAIL")
                    return ok
            except Exception as e:
                LOG.warning("Dispatch crawl job fail: %s | vid=%s", e, vid)
                return False

        _crawler = IdleScheduler(_config, _state, CACHE_FILE, _dispatch_crawl_job)
        _crawler.start()
        LOG.info("Idle crawler STARTED (enabled=%s)", _CRAWLER_ENABLED)
    except Exception as exc:
        LOG.exception("Idle crawler init fail: %s", exc)
        _crawler = None
else:
    LOG.info("Idle crawler DISABLED (CRAWLER_ENABLED=%s, available=%s)",
             _CRAWLER_ENABLED, _crawl_available)

# Fix WebSocket 403 when browser/nginx sends Origin header
try:
    from starlette.middleware import BaseHTTPMiddleware
    from starlette.requests import Request
    from starlette.responses import PlainTextResponse

    class _IgnoreOriginMiddleware(BaseHTTPMiddleware):
        async def dispatch(self, request: Request, call_next):
            return await call_next(request)

    class _StripOriginForWS(BaseHTTPMiddleware):
        async def dispatch(self, request: Request, call_next):
            if request.scope.get("type") == "websocket" and "origin" in request.headers:
                headers = dict(request.scope.get("headers", []))
                headers.pop(b"origin", None)
                request.scope["headers"] = list(headers.items())
            return await call_next(request)

    app.add_middleware(_StripOriginForWS)
except Exception:
    pass



# Fix WebSocket protocol on uvicorn when websocket libs installed
if WebSocket is not None:
    try:
        _ws_impl = importlib.import_module('uvicorn.protocols.websockets.websockets_impl')
    except Exception:
        _ws_impl = None
    try:
        _ws_proto = importlib.import_module('uvicorn.protocols.websockets.wsproto_impl')
    except Exception:
        _ws_proto = None
    if _ws_impl:
        try:
            from uvicorn.protocols.websockets.websockets_impl import WebSocketProtocol
            from uvicorn.config import Config as _UConfig
            _UConfig.reload = False
            _UConfig.ws = "websockets"
        except Exception:
            pass

_lock = threading.Lock()


# ── Helpers ───────────────────────────────────────────────────────────────────

def _load_cache() -> dict:
    try:
        with open(CACHE_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _save_cache(cache: dict):
    tmp = str(CACHE_FILE) + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cache, f, ensure_ascii=False, indent=2)
    os.replace(tmp, str(CACHE_FILE))


def _should_override(existing: dict, incoming: dict) -> bool:
    """Deep is authoritative: Deep replaces Standard, never the reverse.

    Within the same mode, the newest timestamp wins. Legacy entries without
    ``_mode`` are treated as Standard for backward compatibility.
    """
    existing_mode = str(existing.get("_mode", "standard")).lower()
    incoming_mode = str(incoming.get("_mode", "standard")).lower()
    if incoming_mode == "deep" and existing_mode != "deep":
        return True
    if incoming_mode != "deep" and existing_mode == "deep":
        return False
    return int(incoming.get("_ts", 0) or 0) >= int(existing.get("_ts", 0) or 0)


def _check_auth(x_api_key: str | None = None):
    # PATCHED: phan quyen theo route nam o middleware _sec_route_scope; o day chi can key hop le.
    if not _sec_any_valid_key(x_api_key):
        raise HTTPException(status_code=401, detail="Invalid API key")


def _is_allowed_title(title: str) -> bool:
    normalized = " ".join(str(title or "").lower().split())
    return bool(normalized) and "liên khúc" not in normalized and ("karaoke" in normalized or "beat" in normalized)


def _merge_worker_result_into_cache(pending: dict, payload: dict) -> bool:
    """Persist a successful remote-worker result in the shared cache."""
    video_id = str(pending.get("video_id", "")).strip()
    title = str(pending.get("title", "")).strip()
    root = str(payload.get("root", "")).strip()
    scale = str(payload.get("scale", "")).strip()
    viet = str(payload.get("viet", "")).strip()
    western = str(payload.get("western", "")).strip()
    if not (_VIDEO_ID_RE.fullmatch(video_id) and _is_allowed_title(title)
            and root and scale and viet and western):
        return False
    entry = {
        "title": title,
        "root": root,
        "scale": scale,
        "key_scale": f"{root} {scale.capitalize()}",
        "viet": viet,
        "western": western,
        "_ts": int(time.time()),
        "_mode": str(pending.get("mode") or "standard"),
    }
    with _lock:
        cache = _load_cache()
        if video_id not in cache or _should_override(cache[video_id], entry):
            cache[video_id] = entry
            _save_cache(cache)
            return True
    return False


_VIDEO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{6,64}$")
_AUDIO_YTDLP = os.environ.get("AUDIO_YTDLP", "/opt/tone-cache-server/venv/bin/yt-dlp")
_AUDIO_FFMPEG = os.environ.get("AUDIO_FFMPEG", "/usr/bin/ffmpeg")

# ===== AUDIO FALLBACK: concurrency limit 2 + queue + timing logs =====
import logging as _lg
_audio_sem = asyncio.Semaphore(2)   # toi da 2 download audio dong thoi
_audio_log = _lg.getLogger("audio-fallback")
_audio_log.setLevel(_lg.INFO)
_AudioLog_PATH = Path("/opt/tone-cache-server/audio_fallback.log")
try:
    import logging.handlers as _lh
    _h = _lh.TimedRotatingFileHandler(_AudioLog_PATH, when="D", backupCount=2, encoding="utf-8")
    _h.setFormatter(_lg.Formatter("%(asctime)s %(message)s"))
    _audio_log.addHandler(_h)
    _audio_log.propagate = False
except Exception:
    pass

def _prune_audio_log():
    """Xoa log cu sau 2 ngay (TimedRotating da xu ly, day la buong phong)."""
    try:
        if _AudioLog_PATH.exists():
            import time as _t
            age = _t.time() - _AudioLog_PATH.stat().st_mtime
            if age > 2*86400:
                _AudioLog_PATH.unlink()
    except Exception:
        pass


@app.get("/api/audio/{video_id}")
async def download_standard_audio(video_id: str, background_tasks: BackgroundTasks, x_api_key: str | None = Header(None)):
    """Download the Standard 0:20-0:50 clip for desktop fallback."""
    _check_auth(x_api_key)
    if not _VIDEO_ID_RE.fullmatch(video_id):
        raise HTTPException(status_code=400, detail="Invalid video ID")
    _t_queue0 = time.time()
    await _audio_sem.acquire()
    _t_start = time.time()
    _wait_s = _t_start - _t_queue0
    _prune_audio_log()
    _audio_log.info("START video=%s wait=%.2fs active_downloads=%d", video_id, _wait_s, 2 - _audio_sem._value if hasattr(_audio_sem, "_value") else -1)
    # Không cache file audio — kết quả phân tích đã nằm trong tone_cache.
    # Tải vào temp dir, trả file, dọn sạch sau khi response xong.
    tmp_dir = Path(tempfile.mkdtemp(prefix="audio_"))
    background_tasks.add_task(shutil.rmtree, tmp_dir, True)
    base = tmp_dir / ("tw_" + video_id)
    cmd = [_AUDIO_YTDLP, "--no-playlist", "-f", "bestaudio[ext=m4a]/bestaudio[ext=webm]/bestaudio",
           "--no-progress", "--newline", "--concurrent-fragments", "1",
           "--ffmpeg-location", _AUDIO_FFMPEG, "--download-sections", "*0:20-0:50",
           "--extractor-args", "youtube:player_client=default", "-o", str(base) + ".%(ext)s",
           "https://www.youtube.com/watch?v=" + video_id]
    def _run_once() -> subprocess.CompletedProcess | None:
        try:
            return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=10, check=False)
        except (OSError, subprocess.TimeoutExpired):
            return None

    def _find_audio():
        return next((p for p in tmp_dir.glob(base.name + ".*")
                     if p.suffix.lower() in {".m4a", ".webm", ".opus", ".aac", ".mp3", ".ogg"} and p.stat().st_size >= 10000), None)

    def _is_signature_error(stderr: bytes | str) -> bool:
        text = stderr.decode("utf-8", "replace") if isinstance(stderr, bytes) else (stderr or "")
        return ("403" in text) or ("sign in to confirm" in text) or ("nsig extraction" in text) or ("sig" == text.strip()[-3:] if text else False)

    def _try_update_ytdlp() -> bool:
        """Chay pip update nightly trong lock. Tra True neu version doi."""
        global _YTDLP_VERSION
        if not _ytdlp_update_lock.acquire(blocking=False):
            return False  # mot request khac dang update
        try:
            before = _get_ytdlp_version()
            r = subprocess.run([_PIP, "install", "-q", "--pre", "--upgrade", "yt-dlp"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=180, check=False)
            after = _get_ytdlp_version()
            return r.returncode == 0 and after != before
        except Exception:
            return False
        finally:
            _ytdlp_update_lock.release()

    result = None
    audio = None
    last_stderr = b""
    for attempt in range(1, 4):
        result = _run_once()
        last_stderr = getattr(result, "stderr", b"") or b""
        audio = _find_audio() if (result is not None and result.returncode == 0) else None
        if audio is not None:
            break
        if attempt < 3:
            shutil.rmtree(tmp_dir, ignore_errors=True)
            tmp_dir.mkdir(parents=True, exist_ok=True)
            base = tmp_dir / ("tw_" + video_id)
            _audio_log.warning("RETRY video=%s attempt=%d/3", video_id, attempt + 1)

    # Audio fallback: 1 lần đầu + tối đa 2 retry.
    # Sau khi hết retry mà vẫn lỗi chữ ký/403 thì kick update yt-dlp nền.
    if audio is None and _is_signature_error(last_stderr):
        logging.getLogger("tone-cache").warning(
            "yt-dlp sig/403 error after retries (video=%s) — background nightly update queued", video_id)
        threading.Thread(target=_try_update_ytdlp, daemon=True).start()

    if audio is None and result is not None and _is_signature_error(result.stderr):
        logging.getLogger("tone-cache").warning(
            "yt-dlp sig/403 error (rc=%d) — background nightly update queued", result.returncode)
        threading.Thread(target=_try_update_ytdlp, daemon=True).start()

    if audio is None:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        _audio_log.info("FAIL video=%s elapsed=%.2fs", video_id, time.time() - _t_start)
        _audio_sem.release()
        raise HTTPException(status_code=502, detail="VPS audio download failed")
    media = {".m4a": "audio/mp4", ".webm": "audio/webm", ".opus": "audio/ogg", ".aac": "audio/aac", ".mp3": "audio/mpeg", ".ogg": "audio/ogg"}
    _audio_log.info("OK video=%s wait=%.2fs elapsed=%.2fs size=%.1fKB", video_id, _wait_s, time.time() - _t_start, audio.stat().st_size/1024)
    _audio_sem.release()
    return FileResponse(str(audio), media_type=media.get(audio.suffix.lower(), "application/octet-stream"), filename=video_id + audio.suffix)

# ── Models ────────────────────────────────────────────────────────────────────

import logging

_ytdlp_update_lock = threading.Lock()
_PIP = os.environ.get("AUDIO_PIP", "/opt/tone-cache-server/venv/bin/python -m pip").split()

def _get_ytdlp_version() -> str:
    try:
        r = subprocess.run([_AUDIO_YTDLP, "--version"], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=15, check=False)
        return r.stdout.decode("utf-8", "replace").strip()
    except Exception:
        return ""


# ======================================================================
# Remote worker broker (WebSocket outbound -> VPS)
# ======================================================================
import uuid as _uuid
from typing import Optional

_ws_lock = threading.Lock()
_ws_worker = None  # legacy singleton — giữ cho backward-compat, không còn dùng để dispatch
_ws_workers: list = []  # danh sách worker đang online (multi-worker)
_job_pending: dict[str, dict] = {}  # job_id -> {video_id, url, title, mode, caller_ws, created}
_job_result: dict[str, dict] = {}   # job_id -> result payload (used once)
_job_lock = threading.Lock()
_job_counter = 0

def _job_touch():
    global _job_counter
    _job_counter += 1
    return _job_counter

class _CallerSocket:
    def __init__(self, ws):
        self._ws = ws
        self.alive = True

    async def send_json(self, payload: dict):
        try:
            await self._ws.send_json(payload)
        except Exception:
            self.alive = False


class _WorkerSocket:
    def __init__(self, ws, kind: str = "pc"):
        self._ws = ws
        self.alive = True
        self._id = str(_uuid.uuid4())
        self.kind = kind
        self.active_jobs = 0
        self.max_jobs = 1

    async def send_json(self, payload: dict):
        try:
            await self._ws.send_json(payload)
        except Exception:
            self.alive = False


async def _worker_auth(ws, token: str | None) -> bool:
    if not _valid_worker_token(token):
        await ws.close(code=4001)
        return False
    return True

app.title = "PT Studio Tone Cache Server"

@app.websocket("/ws/worker")
async def ws_worker(websocket: WebSocket):
    token = websocket.query_params.get("token")
    if not _valid_worker_token(token):
        await websocket.close(code=4001)
        return
    await websocket.accept()
    worker = _WorkerSocket(websocket, kind=_worker_kind(token))
    with _ws_lock:
        global _ws_worker
        _ws_worker = worker
        if worker not in _ws_workers:
            _ws_workers.append(worker)
        _n_online = len(_ws_workers)
    LOG.info("WORKER CONNECTED kind=%s id=%s online=%d", worker.kind, worker._id[:8], _n_online)
    try:
        await websocket.send_json({"type": "worker_ready"})
        async for raw in websocket.iter_text():
            if not raw:
                continue
            try:
                msg = json.loads(raw)
            except Exception:
                continue
            if not isinstance(msg, dict):
                continue
            t = str(msg.get("type", ""))
            job_id = str(msg.get("job_id", ""))
            if t == "job_ack" and job_id:
                with _ws_lock:
                    worker.active_jobs = max(0, int(msg.get("active_jobs", worker.active_jobs)))
                    worker.max_jobs = max(1, int(msg.get("max_jobs", worker.max_jobs)))
            elif t == "worker_status":
                with _ws_lock:
                    worker.active_jobs = max(0, int(msg.get("active_jobs", worker.active_jobs)))
                    worker.max_jobs = max(1, int(msg.get("max_jobs", worker.max_jobs)))
            elif t == "job_result" and job_id:
                payload = {k: msg.get(k) for k in ("video_id", "root", "scale", "viet", "western", "confidence_pct") if msg.get(k) is not None}
                with _job_lock:
                    _job_result[job_id] = {"payload": payload, "ts": time.time()}
                    pending = _job_pending.pop(job_id, None)
                LOG.info("JOB RESULT job_id=%s video=%s root=%s from_worker=%s",
                         job_id, msg.get("video_id"), msg.get("root"), worker.kind)
                if pending and payload.get("root"):
                    # Auto-save worker result to shared tone-cache
                    try:
                        if _merge_worker_result_into_cache(pending, payload):
                            logging.getLogger("tone-cache").info("tone-cache auto-saved from worker: video=%s title=%s",
                                    pending.get("video_id"), pending.get("title"))
                    except Exception as exc:
                        logging.getLogger("tone-cache").warning("tone-cache auto-save failed: video=%s error=%s",
                                   pending.get("video_id"), exc)
                    ws = pending.get("caller_ws")
                    if isinstance(ws, _CallerSocket) and ws.alive:
                        await ws.send_json({"type": "job_result", "job_id": job_id, "status": "ok", "result": payload})
            elif t == "job_ack":
                pass
    except Exception:
        pass
    finally:
        with _ws_lock:
            if _ws_worker is worker:
                _ws_worker = None
            if worker in _ws_workers:
                _ws_workers.remove(worker)
            _n_online = len(_ws_workers)
        LOG.info("WORKER DISCONNECTED kind=%s id=%s online=%d", worker.kind, worker._id[:8], _n_online)


@app.post("/api/worker/job")
async def create_worker_job(payload: dict = Body(...), background_tasks: BackgroundTasks = None):
    if not isinstance(payload, dict):
        raise HTTPException(status_code=400, detail="invalid payload")
    video_id = str(payload.get("video_id", "")).strip()
    if not _VIDEO_ID_RE.fullmatch(video_id):
        raise HTTPException(status_code=400, detail="invalid video_id")
    url = str(payload.get("url") or f"https://www.youtube.com/watch?v={video_id}")
    title = str(payload.get("title") or "")
    if not _is_allowed_title(title):
        raise HTTPException(status_code=422, detail="Rejected: title must contain karaoke or beat (and not contain liên khúc)")
    mode = str(payload.get("mode") or "standard").lower()
    if mode not in ("standard", "deep"):
        mode = "standard"

    # Phân biệt job user vs job crawler: job user reset đồng hồ idle của crawler.
    _is_crawler_job = str(payload.get("source", "")).lower() == "crawler"
    if not _is_crawler_job and _crawler is not None:
        try:
            _crawler.record_user_job()
        except Exception:
            pass

    with _ws_lock:
        online = [w for w in _ws_workers if w.alive and w.active_jobs < w.max_jobs]
        if not online:
            online = [w for w in [_ws_worker] if w and w.alive and w.active_jobs < w.max_jobs]
        if not online:
            raise HTTPException(status_code=503, detail="all workers busy")
        if _is_crawler_job:
            # Crawler: chia đều job → chọn worker có ít active_jobs nhất.
            # Nếu bằng nhau, ưu tiên NAS (giữ PC rảnh cho user).
            online_sorted = sorted(online, key=lambda w: (w.active_jobs, 0 if w.kind == "nas" else 1))
            worker = online_sorted[0]
        else:
            # User job: PC luôn ưu tiên; NAS chỉ dùng khi PC không còn slot.
            pc = [w for w in online if w.kind == "pc"]
            worker = (pc or online)[0]
        worker.active_jobs += 1
        # Nếu worker bị mất ngay lúc gửi, lần gọi sau sẽ failover sang worker khác.
        _chosen = (worker.kind, worker._id[:8], worker.active_jobs)
    LOG.info("JOB DISPATCHED video=%s mode=%s crawler=%s -> worker kind=%s id=%s active=%d",
             video_id, mode, _is_crawler_job, _chosen[0], _chosen[1], _chosen[2])

    job_id = f"{int(time.time())}-{_job_touch()}-{video_id}"
    with _job_lock:
        _job_pending[job_id] = {"video_id": video_id, "url": url, "title": title,
                                "mode": mode, "created": time.time(), "caller_ws": None}

    async def _run():
        # Fix typing: type ignore to avoid strict websockets import type issues
        await worker.send_json({"type": "job_request", "job_id": job_id, "video_id": video_id, "url": url, "title": title, "mode": mode})  # type: ignore
    if background_tasks is not None:
        background_tasks.add_task(_run)
    else:
        await _run()

    return {"status": "queued", "job_id": job_id, "worker": True}


@app.get("/api/worker/result/{job_id}")
async def get_worker_result(job_id: str, timeout: float = Query(default=30, le=120)):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with _job_lock:
            hit = _job_result.pop(job_id, None)
        if hit and isinstance(hit.get("payload"), dict) and hit["payload"].get("root"):
            return {"status": "ok", "result": hit["payload"]}
        await asyncio.sleep(0.2)
    return {"status": "pending"}
class SyncRequest(BaseModel):
    entries: dict  # {video_id: {title, root, scale, key_scale, viet, western, _ts, _mode}}


# ── Endpoints ─────────────────────────────────────────────────────────────────

@app.get("/health")
def health():
    return {"status": "ok", "entries": len(_load_cache())}


@app.get("/api/tone-cache/all")
def get_all(x_api_key: str | None = Header(None)):
    _check_auth(x_api_key)
    return _load_cache()


@app.get("/api/tone-cache/check/{video_id}")
def check_entry(video_id: str, x_api_key: str | None = Header(None)):
    _check_auth(x_api_key)
    cache = _load_cache()
    entry = cache.get(video_id)
    if entry is None:
        return {"found": False}
    return {"found": True, "entry": entry}


@app.post("/api/tone-cache/sync")
def sync(req: SyncRequest, x_api_key: str | None = Header(None)):
    _check_auth(x_api_key)

    with _lock:
        cache = _load_cache()
        added = 0
        updated = 0
        skipped = 0

        for vid, entry in req.entries.items():
            if not isinstance(entry, dict):
                skipped += 1
                continue
            if vid in cache:
                if _should_override(cache[vid], entry):
                    cache[vid] = entry
                    updated += 1
                else:
                    skipped += 1
            else:
                cache[vid] = entry
                added += 1

        # Gioi han so luong entry — xoa entry cu nhat
        if MAX_ENTRIES is not None and len(cache) > MAX_ENTRIES:
            sorted_keys = sorted(cache.keys(), key=lambda k: cache[k].get("_ts", 0))
            for k in sorted_keys[:len(cache) - MAX_ENTRIES]:
                cache.pop(k, None)

        _save_cache(cache)

    return {
        "status": "ok",
        "added": added,
        "updated": updated,
        "skipped": skipped,
        "total": len(cache),
    }


# ── Debug: worker state (auth required) ─────────────────────────────────────
@app.get("/api/workers/state")
async def workers_state(x_api_key: str | None = Header(None)):
    """Debug: liệt kê worker đang online + slot + job đang chờ."""
    _check_auth(x_api_key)
    with _ws_lock:
        workers = [{
            "id": w._id[:8],
            "kind": w.kind,
            "alive": w.alive,
            "active_jobs": w.active_jobs,
            "max_jobs": w.max_jobs,
        } for w in _ws_workers]
    with _job_lock:
        pending = [{"job_id": k, "video_id": v.get("video_id"),
                    "age_s": round(time.time() - v.get("created", 0), 1)}
                   for k, v in _job_pending.items()]
    return {"worker_count": len(workers), "workers": workers,
            "pending_count": len(pending), "pending": pending}


# ── Debug: crawler status (auth required) ───────────────────────────────────
@app.get("/api/crawler/status")
async def crawler_status(x_api_key: str | None = Header(None)):
    """Debug endpoint — kiểm tra trạng thái idle crawler (auth required)."""
    _check_auth(x_api_key)
    if _crawler is None:
        return {"enabled": False, "available": _crawl_available}
    try:
        return dict(_crawler.status())
    except Exception as exc:
        LOG.warning("Crawler status fail: %s", exc)
        return {"enabled": False, "error": str(exc)}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)


# --- License Admin: PT Studio ---
from license_admin import make_admin_app as _make_admin
pt_admin = _make_admin(
    prefix="",
    title="PT Studio",
    default_secret="PTSTUDIO_LIC_2025_SECRET_KEY_X9",
    default_html="/opt/tone-cache-server/admin.html",
    default_db="/opt/tone-cache-server/licenses.db",
)

# --- License Admin: DDK Studio ---
ddk_admin = _make_admin(
    prefix="DDK",
    title="DDK Studio",
    default_secret="DDKSTUDIO_LIC_2025_SECRET_KEY_X9",
    default_html="/opt/tone-cache-server/admin_ddk.html",
    default_db="/opt/tone-cache-server/licenses_ddk.db",
)
app.mount("/ddk", ddk_admin)
app.mount("/", pt_admin)
