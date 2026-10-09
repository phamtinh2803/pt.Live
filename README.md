# pt.Live - Live Singing DAW

Ứng dụng DAW đơn giản hóa cho hát live với giao diện tối, hỗ trợ ASIO low-latency và MASTER recording.

## Tính năng chính

- **Giao diện tối (Dark Mode)**: Thiết kế clean, nền tối với các nhóm đối tượng tách bạch màu sắc
- **Track Management**: Stereo In, Mono In, FX Channels, Stereo Out (Master)
- **ASIO Integration**: Low-latency audio qua ASIO drivers Windows
- **VST3 Plugin Support**: Tự động quét & nạp VST3 plugins vào inserts/sends
- **MASTER Recording Only**: Ghi âm tổng hợp MASTER ra file WAV
- **Real-time Monitoring**: Lắng nghe trực tiếp từ mic/live source
- **Vertical VU Meters**: LED bar style L/R channels, 1px gap giữa trái phải

## Yêu cầu hệ thống

- Windows 10/11 (64-bit)
- Visual Studio 2022 Community hoặc Build Tools
- CMake 3.15+
- ASIO-compatible audio interface (Focusrite, RME, Behringer, v.v.)

## Cấu trúc dự án

```
ptLive/
├── CMakeLists.txt           # Build configuration
├── src/                     # Source code
│   ├── MainApplication.cpp/h
│   ├── MainWindow.cpp/h
│   └── ...
├── include/                 # Headers
│   ├── Audio/               # Audio engine & tracks
│   ├── UI/                  # Interface components
│   └── Components/          # Reusable UI elements
└── resources/               # Icons & assets
```

## Build hướng dẫn

### Bước 1: Clone dependencies (nếu cần)

```bash
cd A:\deep\ptLive
# JUCE và Tracktion Engine đã có sẵn tại A:\deep\
```

### Bước 2: Configure CMake

```bash
cd A:\deep\ptLive

# Tạo thư mục build
mkdir build
cd build

# Generate Visual Studio project
cmake -G "Visual Studio 17 2022" -A x64 ..
```

### Bước 3: Build project

```bash
# Build Debug version
cmake --build . --config Debug

# Build Release version
cmake --build . --config Release
```

### Bước 4: Chạy ứng dụng

```bash
# Debug build
.\build\Debug\ptLive.exe

# Release build
.\build\Release\ptLive.exe
```

## Sử dụng cơ bản

### 1. Chọn ASIO Device

- Nhấn nút **"Audio Settings"** trong Transport Bar
- Chọn ASIO driver từ danh sách (VD: Focusrite USB ASIO)
- Adjust buffer size (recommendation: 128 samples cho latency thấp)

### 2. Tạo Tracks

- Nhấn **"+ Add Track"** → Chọn loại track
  - **Mono Input**: Cho microphone/single source
  - **Stereo Input**: Cho stereo line-in sources  
  - **FX Channel**: Cho reverb/delay effects
- Rename tracks bằng cách double-click tên track

### 3. Kết nối Input

- Từ dropdown **"Input"** trên mỗi track
- Chọn ASIO channel tương ứng (VD: "ASIO 1-2")
- Monitor mode tự động bật khi arm recording

### 4. Điều chỉnh Levels

- **Volume Slider**: Drag vertical slider để adjust gain
- **VU Meter**: Theo dõi real-time levels (L/R bars)
- Color zones: Green (safe), Yellow (caution), Red (critical)

### 5. Record Master

- Nhấn nút **"REC"** (red circle) trên Transport Bar
- Bắt đầu hát/performance
- Nhấn **"STOP"** để kết thúc
- File dialog hiện ra → Chọn location để save WAV file

### 6. Inspect Track Details

- Click vào một track strip để select
- Inspector Panel bên phải hiển thị chi tiết:
  - Input selector
  - Volume/Pan controls
  - Plugin slots (EQ, Compressor placeholders)
  - Send knobs đến FX destinations

## Scale Factor

Điều chỉnh kích thước giao diện:

- Menu **"View" → Scale** → Chọn 100%, 150%, hoặc 200%
- Layout resizes proportionally để maintain readability
- Persistent across application restarts

## Troubleshooting

### Không thấy ASIO devices?

1. Verify ASIO driver installed correctly
2. Close other audio applications using the device
3. Restart pt.Live after installing new driver

### Audio crackling/distortion?

- Increase ASIO buffer size (VD: 128 → 256 samples)
- Close background applications consuming CPU
- Check if system meets minimum requirements

### VU meters không update?

- Verify track has LevelMeter plugin loaded
- Check audio signal actually reaching track
- Restart audio device in DeviceManager settings

### Window layout appears wrong at high DPI?

- Ensure Windows Display Scaling is set to recommended value
- Try changing scale factor within pt.Live menu
- Restart application after Windows scale change

## Phát triển

### Thêm track type mới

1. Extend `TrackManager::createAudioTrack()` method
2. Implement custom plugin configuration for new type
3. Update UI components to support new track rendering

### Custom VU meter styling

Modify `include/UI/VUMeter.cpp`:
```cpp
// Change color scheme
VU_GREEN = juce::ColourslimeGreen;  // Example: different green
```

### Extending plugins

- Add plugin slot buttons in `InspectorPanel`
- Link to Tracktion's plugin system
- Create placeholder dialogs for each effect type

## License

MIT License - see LICENSE file for details.

## Credits

- Built with [JUCE](https://juce.com/) framework
- Audio engine powered by [Tracktion Engine](https://github.com/Tracktion/tracktion_engine)
- Inspired by professional studio workflows
- Design based on dark theme principles

---

*Version: 1.0.0*  
*Last Updated: Current session*

## Chế độ kỹ thuật viên (KTV)

Bản gốc KTV và password nằm **trong chính file .pt** (đi theo file, chỉ áp dụng cho file đó). Password chỉ bảo vệ việc *thay đổi dữ liệu KTV*; ngoài ra user muốn làm gì cũng được.

- **File > Kỹ thuật viên (KTV) > Lưu bản gốc KTV**: lần đầu tạo password KTV cho file, rồi lưu file và giữ trạng thái hiện tại làm "bản gốc KTV". Các lần sau cần nhập password.
- **Nút Reset trên thanh tiêu đề** (hiện khi file có bản gốc KTV): user tự bấm để về đúng cài đặt KTV đã lưu, không cần password. pt.Live khởi động lại với file đã khôi phục.
- **Đổi password KTV** / **Gỡ chế độ KTV khỏi file này**: cần password.

Password chỉ lưu dạng hash (có salt). Cơ chế này chống user bấm nhầm / nghịch, không phải bảo mật tuyệt đối (người sửa tay file XML vẫn có thể gỡ).

## Mở lại đúng chế độ

pt.Live nhớ chế độ đầu vào cuối cùng (đầy đủ hoặc thu gọn, theo từng máy) và mở lại đúng chế độ đó. Lúc khởi động, cả hai chế độ đều hiện "pt.Live đang khởi động…".
