/*
    pt.Live - Live Singing DAW
    RemoteLink - TCP server (loopback) giao tiếp với ptStudio

    Chức năng:
    - Lắng nghe 127.0.0.1:47800 (thử +1 nếu bị chiếm, tối đa 20 lần)
    - Ghi link.json (%APPDATA%/ptLive/link.json): {port, pid, token}
    - Chỉ nhận DUY NHẤT 1 client (client thứ 2 bị từ chối)
    - Framing: mỗi message là 1 dòng JSON kết thúc bằng '\n'
    - Client gửi {"t":"hello","token":"..."} -> check token mới accept
    - Khi accept: gửi welcome + state snapshot
    - Lệnh từ ptStudio: set music/mic/reverb/tone/autotune/key, show window, quit

    Thread model:
    - Background thread (juce::Thread): accept + read; gom byte tới '\n' -> parse JSON
    - Network thread KHÔNG động Edit/ValueTree. Mọi action đẩy sang message thread
      qua juce::MessageManager::callAsync, rồi gọi các std::function callback do
      MainWindow cài đặt (MainWindow có quyền truy cập TrackManager/AudioEngine).
    - Gửi message: queue + Mutex, background thread rút ra gửi.
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <string>

class RemoteLink : private juce::Thread
{
public:
    RemoteLink();
    ~RemoteLink() override;

    // Start listener. Returns true nếu mở được socket (port có thể khác initialPort).
    bool start (int initialPort = 47800);

    // Stop, đóng socket, xoá link.json
    void stop();

    bool isRunning()   const noexcept { return running_.load(); }
    int  getPort()     const noexcept { return currentPort_.load(); }
    bool isConnected() const noexcept { return connected_.load(); }

    // Gọi từ message thread khi user bấm nút trên ptLive -> đẩy state xuống ptStudio.
    void broadcastState (bool music, bool mic, bool reverb, int toneValue,
                         bool autotuneOn, const juce::String& key, const juce::String& scale);

    // Cập nhật session path (khi mở/save .pt) để gửi kèm welcome/state.
    void setSessionFile (const juce::File& file);

    // Gửi yêu cầu ẩn/hiện cửa sổ đối phương (dùng khi user bấm nút trên ptLive).
    void sendShowCommand (const juce::String& target, bool visible);

    // Path tới link.json.
    static juce::File getLinkFile();

    //=== Callbacks (do MainWindow gán) =====================================
    // RemoteLink không tự thao tác Tracktion; nó chỉ gọi các callback này trên
    // message thread. MainWindow (có TrackManager/AudioEngine) thực thi thật.
    std::function<void (bool)>                                      setMusicMute;      // BEAT
    std::function<void (bool)>                                      setMicMute;        // MICRO
    std::function<void (bool)>                                      setReverbEnabled;  // FX sends MICRO
    std::function<void (int)>                                       setToneValue;      // SoundShifter
    std::function<void (bool)>                                      setAutotuneBypass; // Autotune insert
    std::function<void (const juce::String&, const juce::String&)>  setKeyScale;       // Autotune key/scale
    std::function<void (const juce::String&, bool)>                 applyWindowVisibility;
    std::function<juce::File()>                                     getSessionFile;
    std::function<void()>                                           onQuitRequested;
    std::function<void()>                                           dumpPlugins;       // chẩn đoán: log plugin/param

private:
    // === Thread ===
    void run() override;

    // === Socket helpers ===
    bool openListener (int port);
    void pushOutgoing (const juce::String& json);    // thread-safe enqueue
    void flushOutgoing();                             // background thread drains queue
    void handleIncomingLine (const juce::String& line);
    void handleCommand (const juce::String& jsonLine); // message thread
    void sendWelcome();
    void dropClient();
    void writeLinkFile();
    void removeLinkFile();

    std::unique_ptr<juce::StreamingSocket> listener_;
    std::unique_ptr<juce::StreamingSocket> client_;

    std::atomic<int>  currentPort_ { 47800 };
    std::atomic<bool> running_ { false };
    std::atomic<bool> connected_ { false };
    std::atomic<bool> stopRequested_ { false };
    std::atomic<bool> authenticated_ { false };   // client đã gửi hello đúng token

    // Cờ "còn sống" cho các lambda callAsync (chạy trên message thread; stop() chạy cũng trên message thread).
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>> (true);

    juce::String token_;                 // random hex, mỗi lần chạy

    // Outgoing queue (message thread -> network thread)
    juce::CriticalSection outLock_;
    juce::StringArray   outQueue_;

    // Incoming byte buffer (network thread only). Là bytes để không cắt đôi ký tự UTF-8.
    std::string inBuffer_;

    // State snapshot (đọc/ghi trên message thread)
    struct State {
        bool music = true, mic = true, reverb = true, autotuneOn = true;
        int  tone = 64;
        juce::String key = "C", scale = "Major";
    } state_;
    juce::String sessionPath_;

    static constexpr int MAX_PORT_TRIES = 20;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemoteLink)
};
