#!/usr/bin/env python3
"""
Bỏ nhận admin token qua query string (?token=) trong license_admin.py (bản cũ trên tone-cache).
Nhận dạng các dạng viết phổ biến; nếu sau khi vá vẫn còn tham chiếu `query_params` tới token -> DỪNG, bảo vá tay.

  python apply_license_admin_token_fix.py /opt/tone-cache-server/license_admin.py            # ghi ra .patched
  python apply_license_admin_token_fix.py /opt/tone-cache-server/license_admin.py --in-place # có .bak-secfix
"""
import re, shutil, sys

EMPTY = r"""['"]{2}"""
PATTERNS = [
    # ... or (request.query_params.get("token") or "")   /   ... or (request.query_params.get("token", ""))
    re.compile(r"""\s+or\s+\(\s*request\.query_params\.get\(\s*['"]token['"]\s*(?:,\s*""" + EMPTY + r"""\s*)?\)\s*(?:or\s*""" + EMPTY + r"""\s*)?\)"""),
    # ... or request.query_params.get("token") [or ""]
    re.compile(r"""\s+or\s+request\.query_params\.get\(\s*['"]token['"]\s*(?:,\s*""" + EMPTY + r"""\s*)?\)(?:\s+or\s*""" + EMPTY + r""")?"""),
]


def die(m):
    print("LOI: " + m, file=sys.stderr); sys.exit(2)


def main():
    if len(sys.argv) < 2:
        die(__doc__)
    path = sys.argv[1]
    src = open(path, encoding="utf-8").read()
    new = src
    n_total = 0
    for pat in PATTERNS:
        new, n = pat.subn("", new)
        n_total += n
    if n_total == 0 and "query_params" not in src:
        print("Không thấy ?token= - có thể đã vá rồi."); return
    # còn sót?
    left = [l.strip() for l in new.splitlines() if "query_params" in l and "token" in l]
    if left:
        die("Còn dòng dùng query_params/token, cần vá tay:\n  " + "\n  ".join(left))
    try:
        compile(new, path, "exec")
    except SyntaxError as e:
        die(f"Sau khi vá bị lỗi cú pháp: {e}")
    if in_place := "--in-place" in sys.argv:
        shutil.copy2(path, path + ".bak-secfix"); target = path
    else:
        target = path + ".patched"
    open(target, "w", encoding="utf-8").write(new)
    print(f"OK ({n_total} chỗ) -> {target}")


if __name__ == "__main__":
    main()
