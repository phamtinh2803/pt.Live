---
name: pt.Live Release
about: Create a new release of pt.Live
title: 'Release: vX.Y.Z'
labels: ''
assignees: ''
---

## Version Information
- **Current version:** 
  - CMakeLists.txt `VERSION X.Y.Z`
  - setup.iss `#define MyAppVersion "X.Y.Z"`
- **Previous version:** 

## Build Status
- [ ] Build completed successfully (Visual Studio / Inno Setup)
- [ ] Installer created at `installer/pt.Live-X.Y.Z.exe`
- [ ] SHA256 hash calculated: 

## Release Checklist
1. ✅ Code changes committed to main branch
2. ⏳ Version bumped in source files
3. ⏳ Build and test installer locally
4. ⏳ Upload installer to GitHub Releases (pre-release for testing)
5. ⏳ Update `release/update.json`:
   ```json
   {
     "version": "X.Y.Z",
     "url": "https://github.com/PT-STUDIO/pt.live/releases/download/vX.Y.Z/pt.Live-X.Y.Z.exe",
     "sha256": "<hash>",
     "notes": "- Feature A\n- Fix B"
   }
   ```
6. ⏳ Commit `release/update.json` and push
7. ⏳ Create Git tag: `git tag -a vX.Y.Z -m "pt.Live vX.Y.Z"`
8. ⏳ Push tags to remote: `git push --tags`
9. ⏳ Publish release on GitHub

## Changelog
### Added
- New features...

### Changed
- Changes...

### Fixed
- Bug fixes...

### Security
- Security improvements...

---

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Code <noreply@anthropic.com>
