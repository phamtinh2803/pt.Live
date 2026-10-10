# pt.Live — Auto-update repository

Repo này **chỉ dùng để phân phối bản cập nhật** cho ứng dụng pt.Live.
Không chứa source code.

## Cách hoạt động

Ứng dụng pt.Live khi khởi động sẽ đọc file:

```
https://raw.githubusercontent.com/phamtinh2803/ptLive.daw/main/release/update.json
```

Nếu `version` trong file **cao hơn** bản đang chạy, app hiện dialog hỏi user có cập nhật không.
Bấm **Cập nhật** → app tự tải installer (có kiểm tra SHA256) → tự cài đè → mở lại.

## Cấu trúc

```
release/
└── update.json      # manifest: version mới nhất + link installer + hash
```

File installer `.exe` **không** để trong repo này mà đính vào
[GitHub Releases](../../releases).

## Phát hành bản mới (ví dụ 0.1.1)

1. Build installer mới → `pt.Live-0.1.1.exe`
2. Tính hash:
   ```powershell
   (Get-FileHash -Algorithm SHA256 .\installer\pt.Live-0.1.1.exe).Hash.ToLower()
   ```
3. Tạo GitHub Release với tag `v0.1.1`, đính kèm `pt.Live-0.1.1.exe`
4. Sửa `release/update.json`:
   ```json
   {
     "version": "0.1.1",
     "url": "https://github.com/phamtinh2803/ptLive.daw/releases/download/v0.1.1/pt.Live-0.1.1.exe",
     "sha256": "<hash ở bước 2>",
     "notes": "- Sửa lỗi A\n- Thêm tính năng B"
   }
   ```
5. Commit và push `release/update.json`

Ngay khi file được push, mọi user đang chạy bản cũ sẽ thấy dialog cập nhật ở lần mở app tiếp theo.

## Định dạng update.json

| Trường | Bắt buộc | Ý nghĩa |
|---|---|---|
| `version` | ✅ | Phiên bản mới, chỉ gồm số và dấu chấm (`0.1.1`) |
| `url` | ✅ | Link **https** trực tiếp tới file installer |
| `sha256` | nên có | 64 ký tự hex, app verify sau khi tải |
| `notes` | không | Nội dung hiển thị trong dialog (plain text) |

## Thu hồi bản lỗi

Sửa `release/update.json` về lại `version` cũ hơn (hoặc bằng bản đang chạy).
Ai đã bấm "Bỏ qua" bản lỗi sẽ không bị ảnh hưởng.
