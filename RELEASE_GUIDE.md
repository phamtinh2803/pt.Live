# pt.Live - Hướng dẫn phát hành và cập nhật tự động

> Repo: https://github.com/PT-STUDIO/pt.live

---

## 🚀 Quy trình phát hành lần đầu (v0.1.0)

### Bước 1: Tạo repository trên GitHub
```
1. Vào: https://github.com/new
2. Repository name: `pt.live`
3. Visibility: Public hoặc Private (tuỳ bạn)
4. KHÔNG chọn: "Add a README", ".gitignore" (đã có sẵn)
5. Nhấn "Create repository"
6. Copy URL remote: `https://github.com/PT-STUDIO/pt.live.git`
```

### Bước 2: Push code lên GitHub
```powershell
cd A:\all2\ptLive

# Thay thế YOUR_GITHUB_TOKEN nếu dùng Personal Access Token
# Hoặc chuẩn bị để login khi push

git remote add origin https://github.com/PT-STUDIO/pt.live.git
git branch -M main
git push -u origin main --force
```

**Lưu ý**: Code đã commit rồi. Chỉ cần push remote là xong.

### Bước 3: Tải installer v0.1.0 lên GitHub Releases
```powershell
# 1. Build installer
cd A:\all2\ptLive
.\build_ok.bat  # Chọn option 2 (Release)
# Sau đó compile setup.iss với Inno Setup -> ra file installer/pt.Live-0.1.0.exe

# 2. Upload lên GitHub Releases
#   - Vào: https://github.com/PT-STUDIO/pt.live/releases
#   - Click "Draft a new release"
#   - Tag version: `v0.1.0` (tạo mới từ branch main)
#   - Title: `pt.Live v0.1.0`
#   - Description: copy nội dung từ release/update.json (notes field)
#   - "This is a pre-release": CHECK nếu muốn test, UNCHECK khi chính thức
#   - Attach file: upload installer/pt.Live-0.1.0.exe
#   - Click "Publish release"
```

### Bước 4: Update release/update.json
Sau khi upload thành công, update link trong file manifest:

```jsonc
{
  "version": "0.1.0",
  // THAY ĐỔI link này cho đúng release của bạn
  "url": "https://github.com/PT-STUDIO/pt.live/releases/download/v0.1.0/pt.Live-0.1.0.exe",
  
  // Tính SHA256 hash của installer:
  // PowerShell: Get-FileHash .\installer\pt.Live-0.1.0.exe | Select-Object -ExpandProperty Hash
  "sha256": "tính_sau_khi_upload",
  
  "notes": "pt.Live 0.1.0 - bản phát hành đầu tiên.\n- Live Singing DAW với giao diện tối\n- ASIO low-latency\n- VST3 plugin support\n- MASTER recording\n- Tự kiểm tra cập nhật khi khởi động"
}
```

```powershell
# Commit và tag
git add release/update.json
git commit -m "Update release manifest for v0.1.0"
git tag -a v0.1.0 -m "pt.Live v0.1.0 - Initial release"
git push && git push --tags
```

**Done!** Từ giờ mọi người download app sẽ được thông báo auto-update.

---

## 🔄 Quy trình cho bản tiếp theo (ví dụ: 0.1.1)

### Checklist

- [ ] **Code changes**: Thêm tính năng/sửa lỗi
- [ ] **Bump version**: Sửa trong 2 chỗ
  ```bash
  # CMakeLists.txt
  VERSION 0.1.1
  
  # setup.iss
  #define MyAppVersion "0.1.1"
  ```
- [ ] **Build**: Chạy `build_ok.bat` → Release build
- [ ] **Inno Setup**: Compile `setup.iss` → `installer/pt.Live-0.1.1.exe`
- [ ] **Compute hash**:
  ```powershell
  $hash = Get-FileHash -Algorithm SHA256 .\installer\pt.Live-0.1.1.exe
  $hash.Hash.ToLower()  # Copy cái này vào update.json
  ```
- [ ] **Update manifest**: Sửa `release/update.json`:
  ```json
  {
    "version": "0.1.1",
    "url": "https://github.com/PT-STUDIO/pt.live/releases/download/v0.1.1/pt.Live-0.1.1.exe",
    "sha256": "HASH_TỪ_BƯỚC_TRÊN",
    "notes": "- Thêm tính năng X\n- Sửa lỗi Y"
  }
  ```
- [ ] **Commit & tag**:
  ```bash
  git add release/update.json
  git commit -m "Update release manifest for v0.1.1"
  git tag -a v0.1.1 -m "pt.Live v0.1.1"
  git push && git push --tags
  ```
- [ ] **Upload to GitHub Releases**:
  - Draft a new release
  - Tag: `v0.1.1`
  - Upload `pt.Live-0.1.1.exe`
  - Publish

**Auto-update kích hoạt ngay lập tức**! User chạy v0.1.0 sẽ thấy dialog v0.1.1 sau ~5s.

---

## 🔧 Auto-update Workflow (GitHub Actions)

Khi bạn tag `git push --tags`, workflow `.github/workflows/release.yml` sẽ tự động:

1. ✅ Validate `release/update.json` là hợp lệ JSON
2. ✅ Check version tag khớp với manifest
3. ⚠️ **Cần có installer uploaded thủ công** trước khi workflow chạy
4. ⚠️ Nếu thiếu file installer → workflow fail (báo để fix)

Cách dùng:
```bash
# Manual step 1: Upload installer bằng tay qua web GitHub
# Manually go to: https://github.com/PT-STUDIO/pt.live/releases
# Create draft release with the installer attached

# Manual step 2: Push tags
git tag -a v0.1.1 -m "..."
git push --tags
```

Workflow sẽ tự thêm metadata vào release (nếu có installer).

---

## 🛠 Troubleshooting

### Q: Người dùng không thấy dialog update?
A: Kiểm tra:
1. `release/update.json` public accessible? (open: `https://raw.githubusercontent.com/PT-STUDIO/pt.live/main/release/update.json`)
2. Version trong manifest > version đang chạy? (`newer()` logic: 0.1.1 > 0.1.0)
3. User chưa skip? (check `%APPDATA%\ptLive\update_state.json`)
4. Có thể check log trong `%APPDATA%\ptLive\log.txt`

### Q: Manifest trả 404?
A: Verify đường dẫn RAW:
```
https://raw.githubusercontent.com/PT-STUDIO/pt.live/main/release/update.json
```
Nếu 404 → commit + push chưa kịp propagate (đợi 2 phút).

### Q: SHA256 mismatch?
A: Hash trong manifest ≠ hash thực tế của installer. Rẽ:
1. Recompute hash
2. Update manifest
3. Redo step "Update manifest" ở trên

---

## 📝 Ghi chú quan trọng

- **KHÔNG commit secrets**: `.gitignore` chặn `_keys_backup/*`, `vps_config.json`, `*.zip`
- **Git LFS không cần thiết**: Installer nên attach trực tiếp vào releases (<2GB)
- **HTTPS only**: Tất cả manifest URLs phải HTTPS
- **Fallback**: Nếu app không tải được installer, user bấm "Hủy" → mở link trình duyệt
- **Token mode**: `vps_config.json` trong released installer CHỈ có `"url"` (không có `"key"`) — API key được thay bằng license token (X-License-Token)

---

## 🤖 Generated with Claude Code

Co-Authored-By: Claude Code <noreply@anthropic.com>

🤖 Generated with [Claude Code](https://claude.com/claude-code)
