/*
    pt.Live - Live Singing DAW
    AudioEngine wrapper around Tracktion Engine + DeviceManager

    Handles:
    - te::Engine initialization
    - ASIO device management (chuẩn Tracktion Engine)
    - Edit creation and transport controls
    - PDC (Plugin Delay Compensation) on/off switch
    - Plugin scanning integration (Task 2.10)

    NOTE: This is NOT a singleton — it is owned by MainWindow, which passes a
    pointer to UI components (e.g. TransportBar::setAudioEngine()).

    THIẾT KẾ MỚI (chuẩn Tracktion Engine):
    - Tracktion tự rebuild WaveInputDevice/WaveOutputDevice qua changeListenerCallback
    - dm.dispatchPendingUpdates() flush async rebuild ngay lập tức
    - Không có DeviceSync phức tạp; dùng DM trực tiếp
    - SparseChannelAdapter GIỮ NGUYÊN (cần cho sparse channels)
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/TracktionHeaders.h"
#include "include/UI/MasterRecorder.h"

class AudioEngine  : private te::TransportControl::Listener,
                     private juce::ChangeListener
{
    class SparseChannelAdapter;              // defined in AudioEngine.cpp (GIỮ NGUYÊN)
public:
    AudioEngine();
    ~AudioEngine();

    // Trạng thái device hiện tại (dùng cho Audio Settings dialog) -------------
    struct DeviceConfig
    {
        bool   open = false;
        juce::String driver;
        double sampleRate = 0.0;
        int    bufferSize = 0;
        juce::Array<double> sampleRates;
        juce::Array<int>    bufferSizes;
        int    inputLatency = 0, outputLatency = 0;
    };
    DeviceConfig getDeviceConfig() const;
    juce::ChangeBroadcaster& getDeviceConfigBroadcaster() { return deviceBroadcaster; }

    // Lifecycle
    // knownPlugins: VST3 descriptions (from the app's PluginScanner cache). They are registered in
    // the engine's own plugin list BEFORE the saved session is loaded, so inserts in it can be
    // re-instantiated.
    // sessionToOpen: a .pt file to load instead of the default session (empty = default session).
    bool initialize (const juce::Array<juce::PluginDescription>& knownPlugins = {},
                     const juce::File& sessionToOpen = {});
    void shutdown();
    bool isReady() const noexcept { return ready; }

    // Core accessors -------------------------------------------------------
    te::Engine* getEngine() const noexcept       { return engine.get(); }
    te::DeviceManager* getDeviceManager() const noexcept
        { return engine ? &engine->getDeviceManager() : nullptr; }
    /** Returns the underlying JUCE AudioDeviceManager (patched into Tracktion). */
    juce::AudioDeviceManager* getJuceDeviceManager() const noexcept
        { return engine ? &engine->getDeviceManager().getAudioDeviceManager() : nullptr; }
    te::Edit* getEdit() noexcept                  { return edit.get(); }

    // Device management (chuẩn Tracktion Engine) ---------------------------
    juce::Array<juce::String> listAvailableASIODevices() const;
    juce::Array<juce::String> listAvailableASIOInputs() const;
    static juce::Array<juce::String> getDefaultVST3Paths();

    // Mở driver đã lưu (chuẩn Tracktion): gọi 1 lần lúc startup.
    // Tracktion rebuild wave devices tự động qua changeListenerCallback + dispatchPendingUpdates().
    // Trả về true khi mở thành công.
    // forceReopen = true: LUÔN đóng rồi mở lại driver (giống hệt reload thủ công), kể cả khi driver đang
    // mở đúng thiết lập. MainWindow dùng nó đúng 1 lần ngay sau khi khởi động xong (xem SettleWaiter).
    bool openSavedASIO (bool forceReopen = false);

    // Name of the driver that could not be opened by the last openSavedASIO() ("" = no problem or never configured).
    juce::String getDriverOpenProblem() const { return driverOpenProblem; }

    // True once the start-up graph rebuild (needed once so the master tap is part of it) is finished.
    // MainWindow waits for it (max ~2.5 s) before play(), so the rebuild never cuts live sound.
    bool isStartupGraphSettled() const noexcept { return startupGraphSettled; }

    // Called on the MESSAGE thread when the audio device reports an error or stops by itself
    // (USB pulled, driver reset...). Never called for restarts made by the app (SetupScope).
    std::function<void (const juce::String&)> onDeviceProblem;

    // Đổi driver (Audio Settings). Tracktion rebuild graph tự động.
    bool selectDriver (const juce::String& driverName);

    // Opens exactly this driver of another device type, never via that type's default device (= ASIO4ALL for ASIO).
    // Returns "" on success, else the error text.
    juce::String openDriverExact (const juce::String& typeName, const juce::String& driverName);

    // Đẩy giá trị xuống driver (Tracktion tự read-back qua changeListenerCallback).
    bool requestSampleRate (double rate);
    bool requestBufferSize (int samples);

    // Tick/untick channel: driver restart 1 lần in-place, Tracktion rebuild graph tự động.
    // Không có reload thủ công.
    bool applyChannelSelection();

    // Channels the driver is opened with = EXACTLY the ones the user ticked in Audio Settings
    // (sparse, like Cubase: e.g. In 1-2, In 11-12, Out 1-2). Nothing else is activated, so a
    // virtual driver such as ASIO Link Pro does not stream (and mix) channels nobody uses.
    // Nothing ticked: no input, and Out 1/2 so the driver still opens and reports its buffer size.
    // Tracktion wants buffers indexed by PHYSICAL channel; SparseChannelAdapter (AudioEngine.cpp)
    // converts JUCE's packed buffers, so a sparse set is safe.
    juce::BigInteger makeStreamChannels (const juce::BigInteger& ticked, bool isInput) const;

    // Every setAudioDeviceSetup() made by the app should be inside one of these, so a restart
    // started by the app is not mistaken for a restart requested by the driver.
    struct SetupScope
    {
        SetupScope();
        ~SetupScope();
    };

    // Buffer size / latency ------------------------------------------------
    int getBlockSize() const noexcept;
    double getBlockLengthMs() const noexcept;
    double getSampleRate() const noexcept;
    bool setBlockSize(int samples);          // 64, 128, 256, 512

    // Transport control ----------------------------------------------------
    bool play();
    bool stop();
    bool togglePlayStop();
    bool isPlaying() const;

    // PDC (Plugin Delay Compensation) ---------------------------------------
    // OFF by default. It is switched on/off by the "PDC" checkbox in the title bar.
    //  - OFF : latency compensation is completely disabled in the engine, whatever plugins
    //          (heavy or not) are on the tracks. Pure live monitoring, no extra delay.
    //  - ON  : the engine delays every other path so that all tracks stay time-aligned
    //          (needed later for per-track recording).
    // Static query: there is a single engine per app, and UI widgets just need to read it.
    void setPDCEnabled (bool shouldBeEnabled);
    static bool isPDCEnabled() noexcept;

    // With PDC ON, a heavy plugin delays the whole mix. It is only FLAGGED (red border in the
    // mixer / inspector), never bypassed or disabled - the user decides what to do with it.
    static constexpr double heavyPluginLatencySeconds = 0.005;       // > 5 ms = "heavy"
    static bool isHeavyPlugin(te::Plugin& plugin);

    // Latency / status -----------------------------------------------------
    float getCpuUsage() const noexcept;
    unsigned int getXRunCount() const noexcept;   // callbacks that took longer than their block (dropouts), since start
    int getTotalLatencySamples() const noexcept { return totalLatencySamples; }
    double getTotalLatencyMs() const noexcept;
    juce::String getLatencyText() const;     // e.g. "11.6ms @ 512 samples"
    juce::String getStatusText() const;

    // Plugin scanning (Task 2.10) ------------------------------------------
    te::PluginManager* getPluginManager() noexcept
        { return engine ? &engine->getPluginManager() : nullptr; }
    juce::KnownPluginList& getKnownPluginList() noexcept { return pluginList; }

    // Make plugin descriptions known to the ENGINE (not only to the UI scanner). Tracktion
    // re-creates an ExternalPlugin by looking it up in its own KnownPluginList - if it is not
    // there, the plugin "fails to load" and has no editor.
    void registerKnownPlugins (const juce::Array<juce::PluginDescription>& descriptions);

    // Session persistence (title bar "File" menu) ---------------------------
    // A session is a .pt file: the Tracktion Edit (tracks, faders, inserts + plugin state, sends,
    // input routing) PLUS the audio device setup (driver, sample rate, buffer size, ticked channels).
    static constexpr const char* sessionExtension = ".pt";

    juce::File getCurrentSessionFile() const noexcept   { return currentSessionFile; }   // {} = untitled
    bool saveSessionTo (const juce::File& file);         // writes <file>.pt; true on success
    bool hasUnsavedChanges();                            // edit changed OR audio setup changed since the last save / load
    void markSessionClean();                             // current state = "saved" (called after load / save)

    bool didRequestedFileFail() const noexcept          { return requestedFileFailed; }
    juce::File getRequestedFile() const noexcept        { return requestedFile; }

    // ASIO device config persistence (driver name / sample rate / buffer size) ---
    // Channel ticks are already persisted by ChannelVisibility; this stores the rest so a
    // restart re-opens the SAME driver at the SAME rate/buffer WITHOUT the user having to
    // enter Audio Settings and press Save.
    struct ASIOConfig
    {
        bool     valid       = false;        // file existed and held at least a driver name
        juce::String driverName;
        double   sampleRate  = 0.0;
        int      bufferSize  = 0;
        bool     hasChannels = false;        // inputs / outputs below are meaningful (stored in a .pt)
        juce::BigInteger inputs, outputs;    // ticked channels (ChannelVisibility format)
    };

    static juce::File getASIOConfigFile();   // %APPDATA%/ptLive/asio_config.xml
    static ASIOConfig loadASIOConfig();      // empty .valid if no file / unreadable
    bool saveASIOConfig();                   // snapshot of the current device; true on success

    // Master recorder ------------------------------------------------------
    ptl::MasterRecorder* getMasterRecorder() noexcept { return masterRecorder.get(); }

    // Playback context change handler (from TransportControl::Listener) -----
    // (Re)installs the master tap on the current playback context. Registered as a listener
    // in initialize(); without that registration the recorder was never connected.
    void playbackContextChanged() override;

    // From juce::ChangeListener: Tracktion's DeviceManager broadcasts when the device changes
    // (rate / buffer / channel / driver). We forward it to deviceBroadcaster so the UI can refresh.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    bool isStartupPhase() const noexcept { return startupPhase.load (std::memory_order_relaxed); }
    void unlockStartupPhase() noexcept { startupPhase.store (false, std::memory_order_relaxed); }

private:
    void createOrLoadEdit();
    ASIOConfig captureASIOConfig();                     // what the open ASIO device runs at + its ticked channels
    bool applyASIOConfig (const ASIOConfig& cfg, bool forceReopen = false);       // open that driver / rate / buffer / channels
    void readSessionAsioFromFile();                     // .pt (plain XML) -> sessionAsio, BEFORE any device opens
    void seedStartupDeviceSetup();                      // saved driver -> Tracktion's audio_device_setup (no default device)
    void readAudioSettingsFromEdit();                   // .pt -> sessionAsio
    void storeAudioSettingsInEdit();                    // current device -> the Edit state (written into the .pt)
    juce::String audioSnapshot();                       // one-line summary used to detect audio-setup changes
    void ensureTapActive(bool externalChange);  // never restarts after the first (startup) attempt; only logs
    bool isReadyForTapRetry() const;

    static void postDeviceEvent (juce::WeakReference<AudioEngine> weak, const juce::String& message, bool checkStillStopped);
    void reportDeviceProblem (const juce::String& message);       // message thread only
    juce::String driverOpenProblem;            // session loaded + startup device open finished + a device really running

    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<ptl::MasterRecorder> masterRecorder;
    bool startupGraphSettled = false;           // the first graph (with the tap) is final: safe to start playing
    bool tapAutoRestartDone = false;            // the single allowed automatic restart was used
    bool tapUnattachedLogged = false;           // "tap not attached" logged once per playback context
    int tapWaitCount = 0;                       // polls while "not ready" (reset by every context change)
    bool tapPollPending = false;                // exactly one waiting timer at a time
    std::unique_ptr<SparseChannelAdapter> channelAdapter;   // sits between JUCE and Tracktion
    juce::KnownPluginList pluginList;

    int totalLatencySamples = 0;
    bool ready = false;

    // Session file state
    juce::File requestedFile;                  // .pt passed to initialize() (may be empty)
    bool requestedFileFailed = false;
    juce::File currentSessionFile;             // the .pt the session is saved to ({} = untitled)
    ASIOConfig sessionAsio;                    // audio setup read from the opened .pt (valid = present)
    juce::String savedAudioSnapshot;           // audioSnapshot() at the last save / load

    std::atomic<bool> startupPhase { true };   // true during initial ASIO open: block track-routing restarts

    juce::ChangeBroadcaster deviceBroadcaster;   // notifies UI when device settings change

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioEngine)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
