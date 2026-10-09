# pt.Live v0.1.0 – version, tự kiểm tra cập nhật, quy trình phát hành

> Trạng thái: code đã viết nhưng **chưa được build/chạy thử** (môi trường viết code không có JUCE, Tracktion, MSVC, Inno Setup).
> Việc đầu tiên: build Release và chạy thử theo mục 5.

---

## 1. Đã thực hiện

### 1.1 Hiển thị `v0.1.0` ở bottom bar
- `include/UI/TransportBar.h`, `src/UI/TransportBar.cpp`: thêm `versionLabel` ở góc phải ngoài cùng, chữ mờ (`textFaint`), rộng 56 px, nằm bên phải ô trạng thái CPU.
- Nội dung lấy từ `JUCEApplicationBase::getApplicationVersion()`.

### 1.2 Version chỉ đặt ở MỘT chỗ trong code
- `CMakeLists.txt`: thêm `VERSION 0.1.0` vào `juce_add_gui_app(...)`.
- `src/MainApplication.cpp`: `getApplicationVersion()` trả về `ProjectInfo::versionString` (trước đây là chuỗi cố định `"0.1.0"`).
- Version này đi vào: bottom bar, thuộc tính file .exe (trước đây là version của Tracktion vì `project(VERSION ...)` đọc từ Tracktion), `app_version` gửi lên license server, và phép so sánh khi kiểm tra cập nhật.

### 1.3 Kiểm tra cập nhật khi khởi động
| File | Nội dung |
|---|---|
| `include/Net/UpdateChecker.h`, `src/Net/UpdateChecker.cpp` (mới) | Luồng nền, có kết nối `VpsClient` riêng. Gọi `GET /ptlive/update.json`, so sánh version, kiểm tra "đã bỏ qua", gọi callback trên message thread. |
| `include/Net/VpsClient.h`, `src/Net/VpsClient.cpp` | Thêm `getPublic()`: GET không gửi `X-API-Key` / `X-License-Token`, vẫn dùng TLS ghim cert. |
| `include/UI/UpdateDialog.h`, `src/UI/UpdateDialog.cpp` (mới) | Dialog cùng phong cách WelcomeDialog: **Cập nhật** / **Hủy** / **Bỏ qua bản này**. Enter = Cập nhật, Esc hoặc [x] = Hủy. |
| `include/MainApplication.h`, `src/MainApplication.cpp` | `startUpdateCheck()`, gọi 5 giây sau khi cửa sổ chính mở; `updateChecker.reset()` ở đầu `shutdown()`. |
| `CMakeLists.txt` | Thêm 2 file .cpp mới vào danh sách nguồn. |

Hành vi:
- Chỉ hiện dialog khi `version` trên server **cao hơn** bản đang chạy **và** chưa bị bỏ qua.
- **Cập nhật**: mở `url` trong trình duyệt mặc định (người dùng tự tải, tự cài). App không tự thoát.
- **Hủy**: lần mở app sau sẽ hỏi lại.
- **Bỏ qua bản này**: ghi vào `%APPDATA%\ptLive\update_state.json`; bản **mới hơn** nữa vẫn được hỏi.
- Mọi lỗi (offline, 404, JSON sai, `url` không phải `https://`, version sai định dạng) đều im lặng, chỉ ghi log `[update] ...` trong log của app. Không bao giờ chặn hoặc làm chậm việc khởi động.
- `url` từ server bị từ chối nếu không phải `https://` hoặc chứa khoảng trắng, dấu nháy, ký tự điều khiển. `notes` chỉ vẽ dưới dạng văn bản thuần.

### 1.4 `setup.iss` (Inno Setup)
Đã đọc toàn bộ. **Không cần sửa logic nào** để chạy tính năng cập nhật, vì script đã có sẵn đúng các thứ cần thiết:
- `AppId` cố định + `UsePreviousAppDir=yes` → bản mới cài đè lên bản cũ, giữ nguyên thư mục (icon đã ghim taskbar không bị nhân đôi).
- `CloseApplications=yes` → Windows tự đóng `ptLive.exe` đang chạy trước khi cài đè.
- Dữ liệu người dùng ở `%APPDATA%\ptLive` (license, log, `update_state.json`) không bị gỡ khi cài lại.
- File ra: `installer\pt.Live-<version>.exe`.

Chỉ thêm **một dòng chú thích** cạnh `#define MyAppVersion` để nhắc phải khớp với CMake và `update.json`. Phiên bản này vẫn là bản nhập tay (xem mục 2, bước 1).

Lưu ý khi build installer: `[Files]` lấy `vps_config.json` và `vps_cert.pem` từ `build\ptLive_artefacts\Release`. Kiểm tra `vps_config.json` ở đó **chỉ có `"url"`** (mẫu: `fix/vps_config.json.template-token-mode`), không có `"key"`.

---

## 2. Quy trình mỗi khi có version mới (ví dụ 0.1.1)

1. Đổi version ở **3 chỗ** (phải giống nhau):
   - `CMakeLists.txt` → `VERSION 0.1.1`
   - `setup.iss` → `#define MyAppVersion "0.1.1"`
   - (ở bước 6) `update.json` → `"version": "0.1.1"`
2. Build Release (chạy lại CMake nếu `build_ok.bat` không tự cấu hình lại). Kiểm tra bottom bar hiện `v0.1.1`.
3. Giữ lại `ptLive.pdb` và `ptLive.map` của bản này (cần để đọc crash dump sau này).
4. Biên dịch `setup.iss` → `installer\pt.Live-0.1.1.exe`. Cài thử đè lên bản cũ trên một máy sạch.
5. Upload `pt.Live-0.1.1.exe` lên nơi lưu file tải, lấy link `https://`.
6. **Cuối cùng**, sửa `/var/www/ptlive/update.json` trên VPS:
   ```json
   {
     "version": "0.1.1",
     "url": "https://<nơi-lưu-file>/pt.Live-0.1.1.exe",
     "notes": "- Sửa lỗi A\n- Thêm B"
   }
   ```
   Ngay khi file này đổi, mọi người dùng đang chạy bản cũ sẽ thấy dialog ở lần mở app tới. Vì vậy chỉ sửa sau khi link tải đã tải thử được.
7. Kiểm tra: `curl -sk https://<IP>/ptlive/update.json`, rồi mở bản cũ xem dialog.

Muốn **thu hồi** một bản lỗi: đưa `update.json` về version trước đó. Người đã bấm "Bỏ qua" bản lỗi không bị ảnh hưởng.

---

## 3. Việc bàn giao – chưa làm được ở môi trường code

### 3.1 Người build (máy có Visual Studio / Inno Setup)
- [ ] Build Release, sửa lỗi biên dịch nếu có (code mới chưa được biên dịch).
- [ ] Kiểm tra `v0.1.0` hiện đúng ở bottom bar ở cả cửa sổ rộng và nhỏ (min 800 px).
- [ ] Thuộc tính file `ptLive.exe` (Chi tiết) hiện đúng 0.1.0.
- [ ] Biên dịch `setup.iss`, cài thử trên máy sạch và cài đè lên bản cũ.
- [ ] Lưu `.pdb` / `.map` của bản phát hành.
- [ ] (Tuỳ chọn) Ký số installer và exe bằng code-signing certificate để tránh cảnh báo SmartScreen.

### 3.2 Người quản trị VPS
- [ ] Tạo `/var/www/ptlive/update.json` (mẫu ở mục 2).
- [ ] Thêm vào server block 443 đang dùng:
  ```nginx
  location = /ptlive/update.json {
      alias /var/www/ptlive/update.json;
      default_type application/json;
      add_header Cache-Control "no-cache";
  }
  ```
  rồi `nginx -t && systemctl reload nginx`. (File nginx thật không có trong project nên chưa áp dụng được.)
- [ ] Kiểm tra `curl -sk https://<IP>/ptlive/update.json` trả 200 và đúng JSON.
- [ ] Nếu đã áp bộ hardening trong `fix/README_DEPLOYMENT.md`, xác nhận đường dẫn `/ptlive/` không bị chặn bởi rule nào khác (rate limit, `location /` chung).

### 3.3 Quyết định cần chốt: nơi lưu file cài đặt
- Cert của VPS đang được ghim theo IP (app truy cập bằng IP, không kiểm tra tên miền), nhiều khả năng là cert tự ký. Link tải trỏ vào chính VPS đó sẽ bị **trình duyệt cảnh báo** vì nút "Cập nhật" mở bằng trình duyệt.
- Nên dùng nơi có HTTPS hợp lệ: GitHub Releases, hoặc một tên miền có Let's Encrypt. Chưa kiểm tra được cert thật nên cần người có quyền xác nhận.

### 3.4 Kiểm thử luồng cập nhật (sau khi 3.1 và 3.2 xong)
- [ ] Đặt tạm `update.json` version cao hơn bản đang chạy → dialog hiện đúng sau ~5 giây.
- [ ] **Cập nhật** mở đúng link; **Hủy** → mở lại app vẫn hỏi; **Bỏ qua bản này** → mở lại app không hỏi; tăng version lần nữa → hỏi lại.
- [ ] Tắt mạng / để `update.json` sai / xoá file: app khởi động bình thường, không dialog, log có dòng `[update] ...`.
- [ ] Tắt app trong lúc đang kiểm tra: thoát nhanh, không treo.
- [ ] Đặt `update.json` bằng đúng version đang chạy → không hiện dialog.
- [ ] Phát bản thật sau khi mọi test trên đạt, rồi trả `update.json` về trạng thái thật.

---

## 4. Giới hạn hiện tại và hướng mở rộng
- "Cập nhật" chỉ mở link tải, không tự tải/cài. Tự tải trong app cần thêm code tải file lớn qua kết nối ghim cert (hiện `VpsClient` giới hạn phản hồi 1 MB) và kiểm tra hash SHA-256 trước khi chạy.
- Chưa có cập nhật "bắt buộc" (ví dụ `min_version`): dialog luôn cho phép Hủy / Bỏ qua.
- Chưa có nút "Kiểm tra cập nhật" thủ công (ví dụ bấm vào chữ `v0.1.0` ở bottom bar); hiện chỉ kiểm tra một lần mỗi lần mở app.
- Chỉ kiểm tra khi khởi động. Nếu app chạy nhiều ngày không tắt thì không hỏi lại.
