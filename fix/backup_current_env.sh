#!/bin/bash
# Backup cấu hình + code + DB trước khi đổi. CHẠY TRÊN VPS (root). Thư mục backup chứa secret -> chmod 700.
set -u
TS=$(date +%Y%m%d_%H%M%S)
DIR=/opt/backup/security_$TS
umask 077
mkdir -p "$DIR" && chmod 700 "$DIR" || { echo "❌ không tạo được $DIR"; exit 1; }

FAIL=0
bk() {  # bk <src> [optional]
    if [ -e "$1" ]; then
        cp -a --parents "$1" "$DIR/" || { echo "❌ lỗi copy $1"; FAIL=1; }
    elif [ "${2:-}" != "optional" ]; then
        echo "⚠ thiếu: $1"
    fi
}

bk /etc/systemd/system/tone-cache.service
bk /etc/systemd/system/ptlicense.service
bk /etc/systemd/system/tone-cache.service.d optional
bk /etc/systemd/system/ptlicense.service.d optional
bk /etc/nginx/nginx.conf
bk /etc/nginx/sites-enabled/tone-cache
bk /opt/tone-cache-server/tone_cache_server.py
bk /opt/tone-cache-server/license_admin.py
bk /opt/ptlicense/api.py
bk /opt/tone-cache-server/tone_cache.json
bk /opt/tone-cache-server/licenses.db
bk /opt/tone-cache-server/licenses_ddk.db
bk /opt/ptlicense/data/licenses.db
bk /opt/ptlicense/data/ed25519_private.pem
bk /opt/ptlicense/data/secret.key

# sqlite: bản sao an toàn khi service đang ghi (file .db copy thẳng có thể hỏng)
if command -v sqlite3 >/dev/null; then
    for db in /opt/tone-cache-server/licenses.db /opt/tone-cache-server/licenses_ddk.db /opt/ptlicense/data/licenses.db; do
        [ -f "$db" ] && sqlite3 "$db" ".backup '$DIR/$(echo "$db" | tr / _).sqlite-backup'" 2>/dev/null
    done
fi

[ $FAIL -eq 0 ] && echo "✅ Backup xong: $DIR" || echo "⚠ Backup có lỗi - KHÔNG tiếp tục"
ls -laR "$DIR" | head -60
exit $FAIL
