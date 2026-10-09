#!/usr/bin/env python3
"""Sinh secret ngẫu nhiên an toàn. Chạy LOCAL. Kết quả chứa bí mật -> KHÔNG commit, KHÔNG dán vào file .md."""
import secrets


def hx(n=32):   # hex 64 ký tự, hợp với X-API-Key
    return secrets.token_hex(n)


def url(n=32):  # urlsafe ~43 ký tự, cho admin token
    return secrets.token_urlsafe(n)


print("=== Thêm vào systemd (systemctl edit tone-cache) ===")
print("[Service]")
print(f"Environment=PTLIVE_API_KEY={hx()}")       # phát cho khách ptLive (vps_config.json)
print(f"Environment=SYNC_API_KEY={hx()}")         # riêng cho /sync + /all, KHÔNG phát cho khách
print("# (systemd KHÔNG hỗ trợ comment cuối dòng - giữ comment ở dòng riêng)")
print("# LEGACY_KEY_ALLOW_SYNC=1: key cũ vẫn /sync,/all (chuyển tiếp). Đổi thành 0 khi ptStudio đã dùng SYNC_API_KEY")
print("Environment=LEGACY_KEY_ALLOW_SYNC=1")
print(f"Environment=LICENSE_ADMIN_TOKEN={url()}")
print(f"Environment=DDK_LICENSE_ADMIN_TOKEN={url()}")
print()
print("=== Thêm vào systemd (systemctl edit ptlicense) ===")
print("[Service]")
print(f"Environment=PTLICENSE_ADMIN_TOKEN={url()}")
print()
print("Lưu ý: TONE_CACHE_API_KEY (key cũ) CHƯA đổi ở bước này; chỉ đổi sau khi mọi khách đã cập nhật.")
print("Lưu ý: LICENSE_HMAC_SECRET / DDK_LICENSE_HMAC_SECRET đổi sẽ làm vô hiệu license đã phát - đừng đổi nếu chưa tính kỹ.")
