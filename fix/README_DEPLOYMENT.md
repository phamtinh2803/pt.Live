# Deploy hardening ptLive VPS — kết quả Option 1

## 1. Review `SECURITY_IMPLEMENTATION_PLAN.md`: đúng hướng, nhưng có 6 chỗ phải sửa

Danh sách 12 lỗ hổng và thứ tự ưu tiên là hợp lý. Đối chiếu với code client trong `vps.zip`, các điểm sau trong plan **sẽ gây lỗi nếu làm nguyên văn**:

| # | Vấn đề trong plan | Hậu quả | Đã sửa thế nào |
|---|---|---|---|
| 1 | Nginx `/api/worker/` rate `5r/m` | Client long-poll `/api/worker/result` mỗi ~3s (`pollWindowSec=3`) ≈ 20 req/phút → bị 429 sau ~15s, **nhận diện key bài hát hỏng**. Client còn retry sau 300ms nên càng dồn 429 | `60r/m burst 30` |
| 2 | `_check_sync`: `x_api_key not in (SYNC_API_KEY, API_KEY)` khi env chưa set → `""` nằm trong tuple | Header `X-API-Key:` rỗng **khớp chuỗi rỗng → qua auth** | Bỏ qua key rỗng + `hmac.compare_digest` |
| 3 | `/sync` và `/all` vẫn nhận `API_KEY` cũ | Key cũ là key **đang phát cho mọi khách** → lỗ hổng #1, #2 **chưa thực sự đóng** cho tới khi bỏ key cũ | Thêm cờ `LEGACY_KEY_ALLOW_SYNC` (1 = chuyển tiếp, 0 = chặn hẳn) |
| 4 | `client_max_body_size 128k` cho `/api/tone-cache/` | `/sync` lô lớn của ptStudio → 413 | `/sync` riêng, 1m |
| 5 | Sửa từng endpoint tay (8 chỗ) | Dễ sót, và `_check_auth` cũ sẽ **từ chối key mới** ở các endpoint chưa đổi | Middleware phân quyền theo route + sửa 1 hàm `_check_auth` |
| 6 | `Environment=...  # comment` trong systemd | systemd không hỗ trợ comment cuối dòng → giá trị sai | Comment để dòng riêng; code chỉ đọc token đầu |

Plan viết đúng ở: client **có** gửi `X-API-Key` cho mọi call ngoài `/api/license/*` (kể cả `worker/job` và `worker/result`), nên bắt buộc key ở 2 endpoint đó **không làm hỏng ptLive**. `admin.html` đã gửi token qua header `X-Admin-Token`, nên bỏ `?token=` an toàn.

## 2. File đã tạo & mức độ kiểm thử

| File | Kiểm thử |
|---|---|
| `patches/api_admin_token_fix.py.patch` | ✅ diff thật từ `api.py` trong zip; `patch -p0` áp dụng + `py_compile` OK |
| `patches/apply_tone_cache_split_keys.py` | ⚠️ File gốc `tone_cache_server.py` **không có trong zip** nên không làm được `.patch`. Script tự định vị bằng AST, idempotent, dừng nếu cấu trúc lạ. Logic phân quyền test 36 case (cả 2 chế độ legacy) trên server giả lập; **chưa chạy với FastAPI thật / file thật** |
| `patches/apply_license_admin_token_fix.py` | ⚠️ Test 3 dạng viết phổ biến + 1 dạng lạ (dừng đúng). Chưa thấy file thật |
| `nginx/tone-cache.new`, `deploy_nginx.sh` | ⚠️ Sandbox không có nginx → **chưa chạy `nginx -t`**. `bash -n` OK |
| `scripts/generate_keys.py`, `backup_current_env.sh` | ✅ chạy/`bash -n` OK |
| `templates/*` | — |

Cách dùng script patch (chạy trên VPS, ghi ra file `.patched` để xem trước):
```bash
python3 apply_tone_cache_split_keys.py /opt/tone-cache-server/tone_cache_server.py
diff -u /opt/tone-cache-server/tone_cache_server.py{,.patched} | less
/opt/tone-cache-server/venv/bin/python -m py_compile /opt/tone-cache-server/tone_cache_server.py.patched
# ổn thì thêm --in-place (tự tạo .bak-secfix)
```

## 3. Cần xác nhận TRƯỚC khi sang Option 2 (tôi không thấy được từ code local)

1. **ptStudio (bản đã phát hành) có gửi `X-API-Key` khi gọi `/api/worker/job` và `/result` không?** Nếu không → bắt buộc key ở 2 endpoint này sẽ làm ptStudio hỏng. Kiểm tra: `grep -n "X-API-Key" ` trong code ptStudio.
2. **NAS worker kết nối WebSocket qua `wss://…:443/ws/worker` hay thẳng `ws://…:8000`?** Nếu thẳng :8000 thì ufw/bind 127.0.0.1 sẽ cắt worker. Nên đổi worker sang 443 trước, rồi mới chặn 8000 (và nên đổi `--host 127.0.0.1` trong systemd thay vì chỉ trông vào ufw).
3. **ptStudio có dùng `/sync` và `/all` với key chung không?** Nếu có, giữ `LEGACY_KEY_ALLOW_SYNC=1` cho tới khi ptStudio dùng `SYNC_API_KEY`.
4. `SECURITY_IMPLEMENTATION_PLAN.md` đang chứa **token/key thật dạng plaintext** (API key, admin token, HMAC secret). Coi như đã lộ: xoá khỏi file/đừng gửi đi đâu, và xoay token admin ngay ở bước rotate. `TONE_CACHE_API_KEY` cũ nằm trong config mọi khách nên coi là công khai.
5. Đổi `LICENSE_HMAC_SECRET` / `DDK_LICENSE_HMAC_SECRET` có thể vô hiệu license đã phát → **không đổi** nếu chưa kiểm tra cơ chế.

## 4. Thứ tự Option 2 (giữ như plan, bổ sung)

```
0. backup_current_env.sh           (dừng nếu báo lỗi)
1. Chạy 2 script patch → .patched → diff → py_compile
2. Áp patch api.py (ptlicense) + tone_cache + license_admin; restart; verify
3. deploy_nginx.sh  (+ theo dõi 429 trong access.log vài phút)
4. generate_keys.py → systemctl edit → restart → verify (mục 5)
5. Phát PTLIVE_API_KEY mới cho khách qua bản cập nhật ptLive
6. Khi ptStudio + khách đã chuyển: LEGACY_KEY_ALLOW_SYNC=0, đổi TONE_CACHE_API_KEY
7. ufw + fail2ban (sau khi xác nhận mục 3.2), rồi SSH hardening (mở session mới verify)
```

## 5. Verify sau khi áp (kỳ vọng)

```bash
H=https://103.211.207.59
curl -sk $H/health                                                    # 200
curl -sk -H "X-API-Key: $PTLIVE" $H/api/tone-cache/check/Eq0dxOeUtjY   # 200
curl -sk -H "X-API-Key: $PTLIVE" $H/api/tone-cache/all                 # 403
curl -sk -H "X-API-Key: $PTLIVE" -X POST $H/api/tone-cache/sync -d '{"entries":{}}' -H 'Content-Type: application/json'  # 403
curl -sk -H "X-API-Key: " $H/api/tone-cache/check/x                    # 401  (header rỗng)
curl -sk -X POST $H/api/worker/job -d '{}' -H 'Content-Type: application/json'                     # 401 (trước đây mở)
curl -sk "$H/api/license/admin/list?token=$ADMIN"                      # 401 (query string bị chặn)
curl -sk -H "X-Admin-Token: $ADMIN" $H/api/license/admin/list          # 200
# rate limit: spam /audio → thấy 429
for i in $(seq 1 15); do curl -sk -o /dev/null -w "%{http_code} " -H "X-API-Key: $PTLIVE" $H/api/audio/x; done
# ptLive thật: mở app, đổi 3-4 bài liên tiếp, xem có bài nào chờ key quá lâu / log "429"
```

## 6. Bảo vệ ptStudio production khi deploy

**Nguyên tắc:** key cũ `TONE_CACHE_API_KEY` vẫn dùng được cho `/sync` + `/all` cho tới khi ptStudio đã chuyển sang `SYNC_API_KEY`. Việc này do cờ `LEGACY_KEY_ALLOW_SYNC` quyết định (mặc định `1`). Không sửa logic này thành "luôn nhận key cũ" — key cũ nằm trong config của mọi khách, nên nhận nó vĩnh viễn trên `/sync` thì ai cũng ghi đè được cache chung.

### Timeline an toàn

| Giai đoạn | `LEGACY_KEY_ALLOW_SYNC` | Việc làm |
|---|---|---|
| A. Deploy | `1` | Áp patch, nginx, thêm `PTLIVE_API_KEY` + `SYNC_API_KEY`. ptStudio không đổi gì, vẫn chạy |
| B. Chuyển ptStudio | `1` | Phát bản ptStudio dùng `SYNC_API_KEY`; theo dõi log xem còn client nào dùng key cũ cho `/sync` |
| C. Khóa | `0` | Khi không còn client cũ: đặt `0`, restart, rồi đổi `TONE_CACHE_API_KEY` |

### CHECKLIST BEFORE deploy Option 2
- [ ] Chạy `backup_current_env.sh`, backup thành công
- [ ] Xác nhận ptStudio gửi `X-API-Key` khi gọi `/api/worker/job` và `/result`
- [ ] Xác nhận NAS worker kết nối qua `wss://…:443/ws/worker` (không phải thẳng `:8000`)
- [ ] Xác nhận ptStudio dùng `/sync` và `/all` bằng key nào
- [ ] Giữ `LEGACY_KEY_ALLOW_SYNC=1`

### CHECKLIST VERIFY sau deploy
- [ ] `/sync` và `/all` với `TONE_CACHE_API_KEY` → 200 (đang ở LEGACY=1)
- [ ] `/api/tone-cache/check/<id>` với `PTLIVE_API_KEY` → 200
- [ ] `/all` và `/sync` với `PTLIVE_API_KEY` → 403
- [ ] Mở ptLive thật, đổi 3-4 bài liên tiếp, không có 429
- [ ] Theo dõi `access.log` / `error.log` nginx vài phút

### Cảnh báo
- **Không đổi `LICENSE_HMAC_SECRET` / `DDK_LICENSE_HMAC_SECRET`** nếu chưa kiểm tra cơ chế: có thể vô hiệu hóa license đã phát.
- Token/key thật không được để plaintext trong tài liệu; xoay admin token khi rotate.
