#!/bin/bash
# Backup -> thay config -> nginx -t -> reload. Tự rollback nếu nginx -t fail. CHẠY TRÊN VPS.
# Dùng: bash deploy_nginx.sh /tmp/tone-cache.new
set -uo pipefail
NEW="${1:-/tmp/tone-cache.new}"
TARGET=/etc/nginx/sites-enabled/tone-cache
BK=/root/tone-cache.nginx.orig.$(date +%Y%m%d_%H%M%S)

[ -f "$NEW" ]    || { echo "❌ Không thấy $NEW"; exit 1; }
[ -e "$TARGET" ] || { echo "❌ Không thấy $TARGET"; exit 1; }

# sites-enabled thường là symlink -> ghi ĐÈ nội dung file đích, không thay symlink
REAL=$(readlink -f "$TARGET")
cp -a "$REAL" "$BK" || { echo "❌ Backup fail"; exit 1; }
echo "✔ Backup: $BK"

cp "$NEW" "$REAL"
if nginx -t; then
    systemctl reload nginx && echo "✅ nginx reloaded" || { echo "❌ reload fail -> rollback"; cp -a "$BK" "$REAL"; nginx -t && systemctl reload nginx; exit 1; }
else
    echo "❌ nginx -t FAIL -> rollback"
    cp -a "$BK" "$REAL"
    nginx -t && echo "✔ đã khôi phục config cũ"
    exit 1
fi
echo "Kiểm tra nhanh:"
curl -sk -o /dev/null -w "  /health -> %{http_code}\n" https://127.0.0.1/health
