# Hướng dẫn Tạo Repository GitHub cho pt.Live

## ✅ Đã Hoàn Thành (Local Git)

### Commit History
```
Commit 1 (d425b31): pt.Live v0.1.0 - release with auto-update infrastructure
- Initial codebase commit (108 files, 35KB+)
- Includes all source, config, build scripts
- Auto-excludes secrets via .gitignore

Commit 2 (cd2a26f): Add GitHub-based auto-update for pt.live repo  
- Updated UpdateChecker to fetch manifest from GitHub raw URL by default
- Added setManifestUrl() method for self-hosted VPS support
- Default URL: https://raw.githubusercontent.com/PT-STUDIO/pt.live/main/release/update.json
- release/update.json created as initial manifest (v0.1.0)

Staged but not yet committed (waiting for classifier timeout fix):
- .github/PULL_REQUEST_TEMPLATE.md
- .github/workflows/release.yml  
- RELEASE_GUIDE.md
```

### Git Status
- **Branch:** `master`
- **Tags:** `v0.1.0`
- **Current HEAD:** cd2a26f (2 commits ahead of origin/master)
- **Uncommitted staged files:** `.github/*`, `RELEASE_GUIDE.md`

---

## 📋 Bước Tiếp Theo - Tạo Remote Repository

### Step 1: Create Repository trên GitHub Web
```
1. Truy cập: https://github.com/new
2. Repository name: pt.live
3. Visibility: Public hoặc Private (tuỳ chọn)
4. ĐỪNG tick: "Add README", ".gitignore", "choose a license"
5. Nhấn "Create repository"
6. Copy URL remote hiện tại: 
   https://github.com/<YOUR_ORG>/pt.live.git
   (nếu là PT-STUDIO thì đúng là https://github.com/PT-STUDIO/pt.live.git)
```

### Step 2: Push Local Branch Upstream
```powershell
# Navigate to project directory
cd A:\all2\ptLive

# Replace below with your actual remote URL from Step 1
git remote add origin https://github.com/PT-STUDIO/pt.live.git

# Force push everything (tags + branch)
git push -u origin master --force

# This will output:
# fatal: refusing to include uncommitted changes [...]
```

**Important**: Có 3 files đang stage nhưng chưa commit. Để commit trước khi push:

#### Option A: Manual Commit (khuyến nghị)
Mở terminal PowerShell riêng và run:
```powershell
cd A:\all2\ptLive
git commit -m "Add release workflow, PR template and release guide for auto-update"
git push -u origin master --force
```

#### Option B: Skip Commit Files
Nếu không muốn commit thêm bây giờ, chỉ cần push những gì đã có:
```powershell
# Reset index but keep working tree unchanged
git reset HEAD

# Then push only committed history
git push -u origin master
```

### Step 3: Tag Push
```powershell
# Already tagged locally - just push tags now
git push --tags
```

---

## 🎯 Sau Khi Push Xong

### Verify Everything
```powershell
# Check branch on GitHub:
# https://github.com/PT-STUDIO/pt.live

# Verify files are there (should see):
# - .gitignore ✓
# - CMakeLists.txt ✓
# - README.md ✓
# - release/update.json ✓ ← CRITICAL!
# - setup.iss ✓
# - src/, include/, resources/ folders ✓
```

### Test Auto-Update Manifest
Navigate in browser to:
```
https://raw.githubusercontent.com/PT-STUDIO/pt.live/main/release/update.json
```

Should return JSON:
```json
{
  "version": "0.1.0",
  "url": "https://github.com/PT-STUDIO/pt.live/releases/download/v0.1.0/pt.Live-0.1.0.exe",
  "sha256": "",
  "notes": "pt.Live 0.1.0 - bản phát hành đầu tiên..."
}
```

✅ If this works → app can fetch update.json!

---

## 🚀 Next Steps: Upload Installer & Release

See `RELEASE_GUIDE.md` in repository for complete workflow.

Quick recap:
1. Build installer locally: `installer/pt.Live-0.1.0.exe`
2. Compute SHA256 hash: `Get-FileHash .\installer\pt.Live-0.1.0.exe | Select-Object -ExpandProperty Hash`
3. Update `release/update.json` with real url + sha256
4. Upload installer to GitHub Releases (tagged v0.1.0)
5. Commit manifest change
6. Done!

---

## 🔒 Security Notes

The `.gitignore` protects these from accidental upload:
- `_keys_backup/*` ← Secret keys, NEVER touch
- `vps_config.json` ← API key if exists
- `*.zip` ← Build artifacts, backup files

Always verify before committing:
```bash
git status --short
# Should NOT show anything with underscore or secret
```

---

## 🤖 Generated with Claude Code

Co-Authored-By: Claude Code <noreply@anthropic.com>

🤖 Generated with [Claude Code](https://claude.com/claude-code)
