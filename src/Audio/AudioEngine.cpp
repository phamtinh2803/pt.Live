/*
    pt.Live - Live Singing DAW
    AudioEngine implementation (chuẩn Tracktion Engine)

    Implements:
    - Task 2.1: AudioEngine wrapper class
    - Task 2.2: Device management integration (TRACKTION CHUẨN)
    - Task 2.4: Empty Edit creation (buffer = 512 samples default)
    - Task 2.5: PDC on/off switch (off by default)
    - Task 2.6: Transport controls
    - Task 2.7: MasterRecorder integration
    - Task 2.10: Plugin scanning (3-tier)

    THIẾT KẾ MỚI (Tracktion Engine standard):
    - dm.initialise(2, 2) thay vì (0, 0) → Tracktion xây dựng được WaveInputDevice ngay lần đầu
    - SparseChannelAdapter GIỮ NGUYÊN (cần cho sparse channels)
    - KHÔNG CÓ DeviceSync phức tạp → chỉ dùng SparseChannelAdapter
    - Sau khi mở device → gọi dm.dispatchPendingUpdates() → Tracktion rebuild graph ngay lập tức
    - changeListenerCallback tự động broadcast → UI refresh không cần callback trung gian
*/

#include "include/Audio/AudioEngine.h"
#include "include/UI/MasterRecorder.h"
#include "include/Audio/PluginWindow.h"
#include "include/Audio/PtLog.h"
#include "include/Audio/ChannelVisibility.h"

using namespace tracktion;

namespace {
    std::atomic<bool> pdcEnabledFlag { false };
    std::atomic<int>  setupDepth { 0 };       // > 0 while the APP itself is changing the device setup
    std::atomic<bool> startupPhaseFlag { true };   // true during initial ASIO open: block track-routing restarts

    // A "virtual" ASIO driver (ASIO Link Pro, Voicemeeter, VB-Audio...) has its OWN host
    // buffer / sample rate. If the app opens it forcing a rate/buffer that differs from the
    // host, the driver silently inserts an extra resample / buffering stage and the mic input
    // comes out LATE (audible latency) although the driver's reported latency looks normal.
    // Reloading the driver (Audio Settings) opens it with rate=0 / buffer=0 so it follows the
    // host and the delay disappears. We must do the SAME at start-up for these drivers.
    bool looksLikeVirtualASIODriver (const juce::String& name)
    {
        const auto n = name.toLowerCase().removeCharacters (" -_.");
        return n.contains ("asiolink") || n.contains ("voicemeeter")
            || n.contains ("vbaudio") || n.contains ("virtual") || n.contains ("loopback")
            || n.contains ("blackhole") || n.contains ("flexas") || n.contains ("renewaudio")
            || n.contains ("audiorepeater");
    }
}

AudioEngine::SetupScope::SetupScope()  { ++setupDepth; }
AudioEngine::SetupScope::~SetupScope() { --setupDepth; }

//==============================================================================
// SparseChannelAdapter - lets Tracktion run on a SPARSE set of active ASIO channels.
// (GIỮ NGUYÊN từ phiên bản cũ - không thay đổi gì cả)
class AudioEngine::SparseChannelAdapter : public juce::AudioIODeviceCallback
{
public:
    SparseChannelAdapter (juce::AudioIODeviceCallback* t, AudioEngine* o) : target (t), owner (o) {}

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        auto next = std::make_unique<State>();

        sampleRateHz.store (device != nullptr ? device->getCurrentSampleRate() : 0.0);

        if (device != nullptr)
        {
            next->totalIn  = device->getInputChannelNames().size();
            next->totalOut = device->getOutputChannelNames().size();

            const auto ai = device->getActiveInputChannels();
            const auto ao = device->getActiveOutputChannels();

            for (int i = ai.findNextSetBit (0); i >= 0; i = ai.findNextSetBit (i + 1)) next->inPhys.add (i);
            for (int i = ao.findNextSetBit (0); i >= 0; i = ao.findNextSetBit (i + 1)) next->outPhys.add (i);

            for (int k = 0; k < next->inPhys.size();  ++k) if (next->inPhys[k]  != k) next->dense = false;
            for (int k = 0; k < next->outPhys.size(); ++k) if (next->outPhys[k] != k) next->dense = false;

            for (int k = 0; k < next->inPhys.size();  ++k) if (next->inPhys[k]  >= next->totalIn)  next->valid = false;
            for (int k = 0; k < next->outPhys.size(); ++k) if (next->outPhys[k] >= next->totalOut) next->valid = false;

            const int blockSamples = juce::jmax (16384, device->getCurrentBufferSizeSamples() * 4);
            next->silence.calloc ((size_t) blockSamples);
            next->sink.calloc ((size_t) blockSamples);
            next->scratchSamples = blockSamples;

            next->inScratch.calloc ((size_t) juce::jmax (1, next->totalIn));
            next->outScratch.calloc ((size_t) juce::jmax (1, next->totalOut));
        }

        {
            const juce::ScopedLock sl (lock);
            auto* raw = next.get();
            if (owned != nullptr)
                retired.push_back (std::move (owned));
            owned = std::move (next);
            state.store (raw, std::memory_order_release);

            while (retired.size() > 4)
                retired.erase (retired.begin());
        }

        if (target != nullptr)
            target->audioDeviceAboutToStart (device);
    }

    void audioDeviceStopped() override;

    void audioDeviceError (const juce::String& message) override
    {
        if (target != nullptr)
            target->audioDeviceError (message);

        // Only handed to the message thread: no work here (not on the audio thread's time).
        if (setupDepth.load() == 0)
            AudioEngine::postDeviceEvent (owner, message, false);
    }

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* out, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext& ctx) override
    {
        if (target == nullptr)
            return;

        const auto t0 = juce::Time::getHighResolutionTicks();

        measureInputPeak (in, numIn, numSamples);
        process (in, numIn, out, numOut, numSamples, ctx);

        const double sr = sampleRateHz.load (std::memory_order_relaxed);
        if (sr > 0.0 && numSamples > 0)
        {
            const double usedSeconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);

            if (usedSeconds > (double) numSamples / sr)
                ptl::signalStats().xruns.fetch_add (1, std::memory_order_relaxed);
        }
    }

private:
    static void measureInputPeak (const float* const* in, int numIn, int numSamples) noexcept
    {
        float peak = 0.0f;
        int peakChannel = -1;

        for (int c = 0; c < numIn; ++c)
        {
            if (in[c] == nullptr)
                continue;

            const float p = juce::FloatVectorOperations::findMaximum (in[c], numSamples);
            const float n = -juce::FloatVectorOperations::findMinimum (in[c], numSamples);
            const float a = juce::jmax (p, n);

            if (a > peak) { peak = a; peakChannel = c; }
        }

        auto& st = ptl::signalStats();
        if (peak > st.inPeak.load (std::memory_order_relaxed))
        {
            st.inPeak.store (peak, std::memory_order_relaxed);
            st.inPeakChannel.store (peakChannel, std::memory_order_relaxed);
        }
    }

    void process (const float* const* in, int numIn,
                  float* const* out, int numOut, int numSamples,
                  const juce::AudioIODeviceCallbackContext& ctx)
    {
        juce::ScopedNoDenormals noDenormals;

        for (int k = 0; k < numOut; ++k)
            if (out[k] != nullptr)
                juce::FloatVectorOperations::clear (out[k], numSamples);

        const State* st = state.load (std::memory_order_acquire);

        if (st == nullptr || st->dense || ! st->valid || numSamples > st->scratchSamples
              || numIn != st->inPhys.size() || numOut != st->outPhys.size())
        {
            target->audioDeviceIOCallbackWithContext (in, numIn, out, numOut, numSamples, ctx);
            return;
        }

        State& s = const_cast<State&> (*st);

        for (int i = 0; i < s.totalIn;  ++i) s.inScratch[i]  = s.silence;
        for (int i = 0; i < s.totalOut; ++i) s.outScratch[i] = s.sink;

        for (int k = 0; k < numIn;  ++k) s.inScratch[s.inPhys[k]]   = in[k];
        for (int k = 0; k < numOut; ++k) s.outScratch[s.outPhys[k]] = out[k];

        target->audioDeviceIOCallbackWithContext (s.inScratch, s.totalIn, s.outScratch, s.totalOut, numSamples, ctx);
    }

private:
    struct State
    {
        juce::Array<int> inPhys, outPhys;
        int totalIn = 0, totalOut = 0, scratchSamples = 0;
        bool dense = true;
        bool valid = true;
        juce::HeapBlock<float> silence, sink;
        juce::HeapBlock<const float*> inScratch;
        juce::HeapBlock<float*> outScratch;
    };

    juce::AudioIODeviceCallback* target = nullptr;
    std::atomic<double> sampleRateHz { 0.0 };
    juce::CriticalSection lock;
    juce::WeakReference<AudioEngine> owner;
    std::atomic<State*> state { nullptr };
    std::unique_ptr<State> owned;
    std::vector<std::unique_ptr<State>> retired;
};

void AudioEngine::SparseChannelAdapter::audioDeviceStopped()
{
    if (target != nullptr)
        target->audioDeviceStopped();

    // A stop that the app did not ask for (not inside SetupScope). Checked again on the message
    // thread a moment later: if the device came back by itself nothing is reported.
    if (setupDepth.load() == 0)
        AudioEngine::postDeviceEvent (owner, "audio device stopped", true);
}

void AudioEngine::postDeviceEvent (juce::WeakReference<AudioEngine> weak, const juce::String& message, bool checkStillStopped)
{
    juce::MessageManager::callAsync ([weak, message, checkStillStopped]
    {
        if (weak.get() == nullptr)
            return;

        if (! checkStillStopped)
        {
            weak.get()->reportDeviceProblem (message);
            return;
        }

        juce::Timer::callAfterDelay (800, [weak, message]
        {
            auto* self = weak.get();

            if (self == nullptr || setupDepth.load() > 0)
                return;

            auto* jdm = self->getJuceDeviceManager();
            auto* dev = jdm != nullptr ? jdm->getCurrentAudioDevice() : nullptr;

            if (dev != nullptr && dev->isOpen() && dev->isPlaying())
                return;                                   // it is running again

            self->reportDeviceProblem (message);
        });
    });
}

void AudioEngine::reportDeviceProblem (const juce::String& message)
{
    ptl::diag::write ("[ASIO] device error: " + message);

    if (ready && onDeviceProblem)
        onDeviceProblem (message);
}

//==============================================================================
// Thiết kế đơn giản: không có DeviceSync phức tạp nữa
// Tracktion tự động rebuild wave devices qua changeListenerCallback
// Ứng dụng chỉ cần gọi dm.dispatchPendingUpdates() sau mỗi thay đổi device

AudioEngine::AudioEngine() {}

AudioEngine::~AudioEngine()
{
    shutdown();
}

bool AudioEngine::initialize (const juce::Array<juce::PluginDescription>& knownPlugins,
                               const juce::File& sessionToOpen)
{
    if (ready)
        return true;

    requestedFile = sessionToOpen;

    //== Step 1: Create Engine ==============================================
    engine = std::make_unique<te::Engine> ("pt.Live",
                                           std::make_unique<PtUIBehaviour>(),
                                           nullptr);

    // === QUAN TRỌNG ===
    // initialise(2, 2) thay vì (0, 0) → Tracktion build được WaveInputDevice ngay lần đầu
    // Nếu truyền (0, 0), WaveInputDevice list trống → input channel không active
    auto& dm = engine->getDeviceManager();
    engine->getPropertyStorage().removeProperty (te::SettingID::audio_device_setup);

    // dm.initialise() opens a DEFAULT device when it has no saved setup, and JUCE's default ASIO driver is
    // ASIO4ALL: it was opened and closed again at every start (ptLive.log: "'ASIO4ALL v2' already open - closing
    // it"), which also froze the message thread for seconds. So the saved driver goes into the setup that
    // initialise() reads: the FIRST open is already the right driver and no default device is ever opened.
    readSessionAsioFromFile();
    seedStartupDeviceSetup();

    dm.initialise (2, 2);  // <-- FIX #1: phải có min 2 input/output channels

    // Lắng nghe Tracktion DM broadcast → forward sang deviceBroadcaster cho UI
    dm.addChangeListener (this);

    // SparseChannelAdapter vẫn giữ (cần cho sparse channels)
    {
        auto& adm = dm.getAudioDeviceManager();
        auto* teCallback = (juce::AudioIODeviceCallback*) &dm;
        adm.removeAudioCallback (teCallback);
        channelAdapter = std::make_unique<SparseChannelAdapter> (teCallback, this);
        adm.addAudioCallback (channelAdapter.get());
    }

    // Tracktion tự rebuild wave devices qua changeListenerCallback + dispatchPendingUpdates().
    // Không cần DeviceSync phức tạp nữa.

    //== Step 2: Edit =======================================================
    registerKnownPlugins (knownPlugins);
    createOrLoadEdit();

    //== Step 3: Master recorder ============================================
    if (edit != nullptr)
    {
        masterRecorder = std::make_unique<ptl::MasterRecorder> (*edit);
        edit->getTransport().addListener (this);
        playbackContextChanged();
    }

    // Default latency tracking
    totalLatencySamples = dm.getBlockSize();

    ready = true;
    startupPhaseFlag.store(true);
    return true;
}

void AudioEngine::shutdown()
{
    if (!ready)
        return;

    if (masterRecorder != nullptr)
        masterRecorder->stopRecording();

    stop();

    if (edit != nullptr)
        edit->getTransport().removeListener (this);

    edit.reset();
    masterRecorder.reset();

    if (engine != nullptr)
    {
        auto& dm = engine->getDeviceManager();
        dm.removeChangeListener (this);   // STOP listening to Tracktion DM changes
        dm.closeDevices();

        if (channelAdapter != nullptr)
        {
            engine->getDeviceManager().getAudioDeviceManager().removeAudioCallback (channelAdapter.get());
            channelAdapter.reset();
        }

        engine.reset();
    }

    ready = false;
    startupPhaseFlag.store(false);
}

// =============================================================================
// Mở driver đã lưu (chỉ gọi 1 lần tại startup)
// Tracktion rebuild wave devices TỰ ĐỘNG qua changeListenerCallback + dispatchPendingUpdates()
bool AudioEngine::openSavedASIO (bool forceReopen)
{
    // NOTE: NOT gated on 'ready'. At startup this runs from createOrLoadEdit(), i.e. INSIDE initialize(),
    // while 'ready' is still false (it is set at the very end). Gating on it made the startup open a
    // silent no-op: the saved driver was never opened and every track came back as "No Input".
    if (engine == nullptr)
        return false;

    driverOpenProblem.clear();
    juce::String triedDriver;

    // Ưu tiên .pt file, nếu không có thì dùng asio_config.xml
    if (sessionAsio.valid)
    {
        triedDriver = sessionAsio.driverName;

        if (applyASIOConfig(sessionAsio, forceReopen))
        {
            saveASIOConfig();
            return true;
        }
        ptl::diag::write("[ASIO] driver of .pt not available -> fallback to last config");
    }

    const auto cfg = loadASIOConfig();
    if (cfg.valid)
    {
        if (applyASIOConfig(cfg, forceReopen))
            return true;

        driverOpenProblem = cfg.driverName;
        return false;
    }

    // The .pt asked for a driver and there is no other config to fall back on: that is a real failure.
    if (triedDriver.isNotEmpty())
    {
        driverOpenProblem = triedDriver;
        return false;
    }

    // Never configured -> leave device closed, let user choose in Audio Settings
    ptl::diag::write("[ASIO] no saved config - using Tracktion's current default device (if any)");
    return true;   // not an error
}

// =============================================================================
// Đổi driver trong Audio Settings
bool AudioEngine::selectDriver(const juce::String& driverName)
{
    if (! ready || engine == nullptr)
        return false;

    auto& dm = engine->getDeviceManager();
    auto& adm = dm.getAudioDeviceManager();

    // Find driver type
    juce::String asioTypeName;
    for (auto* t : adm.getAvailableDeviceTypes())
    {
        if (t == nullptr || ! t->getTypeName().toLowerCase().contains("asio"))
            continue;
        t->scanForDevices();
        if (t->getDeviceNames(false).contains(driverName) || t->getDeviceNames(true).contains(driverName))
        {
            asioTypeName = t->getTypeName();
            break;
        }
    }

    if (asioTypeName.isEmpty())
    {
        ptl::diag::write("[ASIO] driver '" + driverName + "' not found");
        return false;
    }

    // Another device type is current (e.g. WASAPI): open the driver EXACTLY, never through the type's
    // default device (JUCE's default ASIO device is ASIO4ALL).
    if (adm.getCurrentAudioDeviceType() != asioTypeName)
    {
        const auto e = openDriverExact(asioTypeName, driverName);

        if (e.isNotEmpty())
            ptl::diag::write("[ASIO] select driver failed: " + e);

        return e.isEmpty();
    }

    AudioEngine::SetupScope scope;     // app-made change, not a driver reset

    // Đóng driver cũ trước khi mở driver mới (giải phóng hardware / virtual routing)
    adm.closeAudioDevice();

    auto setup = adm.getAudioDeviceSetup();
    setup.outputDeviceName = driverName;
    setup.inputDeviceName  = driverName;
    setup.useDefaultInputChannels  = false;
    setup.useDefaultOutputChannels = false;

    // Channels = ticks đã lưu (In 1-2 / Out 1-2 mặc định), sparse như Cubase
    auto& vis = ptl::ChannelVisibility::get();
    vis.initialise(driverName);
    setup.inputChannels  = makeStreamChannels(vis.getInputs(driverName),  true);
    setup.outputChannels = makeStreamChannels(vis.getOutputs(driverName), false);

    setup.bufferSize = 0;   // driver's preferred buffer
    setup.sampleRate = 0.0; // driver's current rate

    // Apply via JUCE DM (Tracktion's change listener sẽ trigger rebuild)
    auto err = adm.setAudioDeviceSetup(setup, true);

    // CRITICAL: Flush async wave-device rebuild NOW
    // Tracktion rebuilds wave inputs/outputs asynchronously after device change
    // Without this, the graph builds with stale wave devices → no sound
    dm.dispatchPendingUpdates();

    if (err.isNotEmpty())
        ptl::diag::write("[ASIO] select driver failed: " + err);

    return err.isEmpty();
}

// =============================================================================
// Opens ONE named driver of a device type that is not the current type, WITHOUT ever opening the type's
// default device first. juce::AudioDeviceManager::setCurrentAudioDeviceType() always opens the default device of
// the new type, and for ASIO JUCE's default is ASIO4ALL: its tray icon / control panel popped up although the
// user had chosen another driver (e.g. ASIO Link Pro). Same open path as applyASIOConfig(): explicit names,
// selectDefaultDeviceOnFailure = false, so a driver that fails to open is reported, never replaced by ASIO4ALL.
// Returns "" on success, else the error text.
juce::String AudioEngine::openDriverExact(const juce::String& typeName, const juce::String& driverName)
{
    auto* jdm = getJuceDeviceManager();

    if (jdm == nullptr || engine == nullptr)
        return "audio engine not ready";

    auto& vis = ptl::ChannelVisibility::get();
    vis.initialise(driverName);

    juce::XmlElement state("DEVICESETUP");
    state.setAttribute("deviceType",            typeName);
    state.setAttribute("audioInputDeviceName",  driverName);
    state.setAttribute("audioOutputDeviceName", driverName);
    state.setAttribute("audioDeviceRate",       0.0);      // the driver's own current rate
    state.setAttribute("audioDeviceBufferSize", 0);        // the driver's own preferred buffer
    state.setAttribute("audioDeviceInChans",    makeStreamChannels(vis.getInputs(driverName),  true ).toString(2));
    state.setAttribute("audioDeviceOutChans",   makeStreamChannels(vis.getOutputs(driverName), false).toString(2));

    juce::String err;
    {
        SetupScope scope;
        jdm->closeAudioDevice();
        err = jdm->initialise(2, 2, &state, false);
    }

    engine->getDeviceManager().dispatchPendingUpdates();
    return err;
}

// =============================================================================
// Request sample rate → push to driver, then read-back
bool AudioEngine::requestSampleRate(double rate)
{
    if (! ready || engine == nullptr)
        return false;

    auto& dm = engine->getDeviceManager();
    auto& adm = dm.getAudioDeviceManager();

    auto setup = adm.getAudioDeviceSetup();
    setup.sampleRate = rate;
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
    setup.bufferSize = 0;

    auto err = adm.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
        ptl::diag::write("[ASIO] sample rate " + juce::String(rate, 0) + " refused: " + err);

    // Dispatch pending updates to ensure graph rebuild with new rate
    dm.dispatchPendingUpdates();

    return err.isEmpty() && std::abs(dm.getSampleRate() - rate) < 1.0;
}

// =============================================================================
// Request buffer size → push to driver, then read-back
bool AudioEngine::requestBufferSize(int samples)
{
    if (! ready || engine == nullptr)
        return false;

    auto& dm = engine->getDeviceManager();
    auto& adm = dm.getAudioDeviceManager();

    auto setup = adm.getAudioDeviceSetup();
    setup.bufferSize = samples;
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;

    auto err = adm.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
        ptl::diag::write("[ASIO] buffer " + juce::String(samples) + " refused: " + err);

    // Dispatch pending updates to ensure graph rebuild
    dm.dispatchPendingUpdates();

    return err.isEmpty();
}

// =============================================================================
// Tick/untick channel selection → driver restarts once in-place, Tracktion rebuilds graph
bool AudioEngine::applyChannelSelection()
{
    if (! ready || engine == nullptr)
        return false;

    auto& dm = engine->getDeviceManager();
    auto& adm = dm.getAudioDeviceManager();

    auto* dev = adm.getCurrentAudioDevice();
    if (dev == nullptr || ! dev->isOpen())
        return false;

    const auto before = adm.getAudioDeviceSetup();
    auto& vis = ptl::ChannelVisibility::get();
    vis.initialise(dev->getName());

    auto setup = adm.getAudioDeviceSetup();
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
    setup.inputChannels  = makeStreamChannels(vis.getInputs(dev->getName()), true);
    setup.outputChannels = makeStreamChannels(vis.getOutputs(dev->getName()), false);

    // Rate/buffer stay unchanged
    setup.sampleRate = dev->getCurrentSampleRate();
    setup.bufferSize = dev->getCurrentBufferSizeSamples();

    if (setup.inputChannels == before.inputChannels && setup.outputChannels == before.outputChannels)
    {
        ptl::diag::write("[ASIO] channel selection unchanged");
        return false;
    }

    auto err = adm.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
        ptl::diag::write("[ASIO] channel change failed: " + err);

    // Dispatch pending updates so Tracktion rebuilds wave devices immediately
    dm.dispatchPendingUpdates();

    return err.isEmpty();
}

juce::BigInteger AudioEngine::makeStreamChannels(const juce::BigInteger& ticked, bool isInput) const
{
    if (! ticked.isZero())
        return ticked;

    juce::BigInteger d;
    if (! isInput)
        d.setRange(0, 2, true);
    return d;
}

int AudioEngine::getBlockSize() const noexcept
{
    auto* jdm = getJuceDeviceManager();
    if (jdm != nullptr)
        if (auto* d = jdm->getCurrentAudioDevice())
            if (d->getCurrentBufferSizeSamples() > 0)
                return d->getCurrentBufferSizeSamples();

    auto* dm = getDeviceManager();
    return dm != nullptr ? dm->getBlockSize() : 0;
}

double AudioEngine::getBlockLengthMs() const noexcept
{
    auto* dm = getDeviceManager();
    return dm != nullptr ? dm->getBlockSizeMs() : (512.0 / 44.1);
}

double AudioEngine::getSampleRate() const noexcept
{
    auto* dm = getDeviceManager();
    return dm != nullptr ? dm->getSampleRate() : 44100.0;
}

bool AudioEngine::setBlockSize(int samples)
{
    if (samples != 64 && samples != 128 && samples != 256 && samples != 512)
        return false;
    totalLatencySamples = samples;
    return true;
}

bool AudioEngine::play()
{
    if (edit == nullptr || ! ready)
        return false;
    edit->getTransport().play(false);
    return true;
}

bool AudioEngine::stop()
{
    if (edit == nullptr || ! ready)
        return false;
    edit->getTransport().stop(true, false);
    return true;
}

bool AudioEngine::togglePlayStop()
{
    if (edit == nullptr || ! ready)
        return false;
    if (edit->getTransport().isPlaying())
        return stop();
    return play();
}

bool AudioEngine::isPlaying() const
{
    auto* e = const_cast<AudioEngine*>(this)->getEdit();
    return e != nullptr && e->getTransport().isPlaying();
}

void AudioEngine::setPDCEnabled(bool shouldBeEnabled)
{
    pdcEnabledFlag.store(shouldBeEnabled);
    ptl::diag::write(juce::String("[PDC] ") + (shouldBeEnabled ? "ON" : "OFF"));
    if (edit != nullptr)
        edit->setLatencyCompensationEnabled(shouldBeEnabled);
}

bool AudioEngine::isPDCEnabled() noexcept { return pdcEnabledFlag.load(); }

bool AudioEngine::isHeavyPlugin(te::Plugin& plugin)
{
    return plugin.getLatencySeconds() > heavyPluginLatencySeconds;
}

unsigned int AudioEngine::getXRunCount() const noexcept
{
    return ptl::signalStats().xruns.load(std::memory_order_relaxed);
}

float AudioEngine::getCpuUsage() const noexcept
{
    auto* dm = getDeviceManager();
    return dm != nullptr ? dm->getCpuUsage() : 0.0f;
}

double AudioEngine::getTotalLatencyMs() const noexcept
{
    const double sr = getSampleRate();
    const double bufferMs = sr > 0.0 ? (getBlockSize() / sr) * 1000.0 : 0.0;
    auto* dm = getDeviceManager();
    const double outMs = dm != nullptr ? dm->getOutputLatencySeconds() * 1000.0 : 0.0;
    return bufferMs + outMs;
}

juce::String AudioEngine::getLatencyText() const
{
    juce::String text;
    text << juce::String(getTotalLatencyMs(), 1) << "ms @ "
         << getBlockSize() << " samples | PDC: " << (isPDCEnabled() ? "On" : "Off");
    return text;
}

juce::String AudioEngine::getStatusText() const
{
    if (!isReady())
        return "Audio engine not ready";
    if (isPlaying())
        return "Monitoring | " + getLatencyText();
    return "Ready | " + getLatencyText();
}

void AudioEngine::registerKnownPlugins(const juce::Array<juce::PluginDescription>& descriptions)
{
    if (engine == nullptr)
        return;
    auto& known = engine->getPluginManager().knownPluginList;
    for (const auto& d : descriptions)
        known.addType(d);
}

//==============================================================================
// getDeviceConfig - snapshot trạng thái device hiện tại (cho Audio Settings dialog)
AudioEngine::DeviceConfig AudioEngine::getDeviceConfig() const
{
    DeviceConfig c;
    auto* jdm = getJuceDeviceManager();   // now const-correct
    if (jdm != nullptr)
    {
        if (auto* d = jdm->getCurrentAudioDevice())
        {
            c.open          = d->isOpen();
            c.driver        = d->getName();
            c.sampleRate    = d->getCurrentSampleRate();
            c.bufferSize    = d->getCurrentBufferSizeSamples();
            c.sampleRates   = d->getAvailableSampleRates();
            c.bufferSizes   = d->getAvailableBufferSizes();
            c.inputLatency  = d->getInputLatencyInSamples();
            c.outputLatency = d->getOutputLatencyInSamples();

            if (c.bufferSize > 0 && ! c.bufferSizes.contains(c.bufferSize))
                c.bufferSizes.addUsingDefaultSort(c.bufferSize);
        }
    }
    return c;
}

// changeListenerCallback - lắng nghe Tracktion DM broadcast (device thay đổi)
// → forward vào deviceBroadcaster để UI refresh
void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Log the device state on EVERY change (seed open, start-up reopen, manual reload, rate/buffer
    // change). Comparing the start-up line with the line after a manual reload is the only way to
    // see what the reload really changes: with a virtual driver (ASIO Link Pro) the reported
    // latencies look identical while the mic is late, so the difference must show up somewhere
    // in these numbers (or nowhere visible - which is also a result).
    if (auto* jdm = getJuceDeviceManager())
        if (auto* dev = jdm->getCurrentAudioDevice())
        {
            const auto setup = jdm->getAudioDeviceSetup();
            ptl::diag::write("[ASIO] device changed: '" + dev->getName()
                             + "' reqBuf=" + juce::String(setup.bufferSize)
                             + " actualBuf=" + juce::String(dev->getCurrentBufferSizeSamples())
                             + " rate=" + juce::String(dev->getCurrentSampleRate(), 0)
                             + " inLat=" + juce::String(dev->getInputLatencyInSamples())
                             + " outLat=" + juce::String(dev->getOutputLatencyInSamples())
                             + " activeIn=" + juce::String(dev->getActiveInputChannels().countNumberOfSetBits())
                             + " activeOut=" + juce::String(dev->getActiveOutputChannels().countNumberOfSetBits()));
        }

    deviceBroadcaster.sendChangeMessage();
}

//==============================================================================
// ASIO device config persistence
juce::File AudioEngine::getASIOConfigFile()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("ptLive");
    dir.createDirectory();
    return dir.getChildFile("asio_config.xml");
}

AudioEngine::ASIOConfig AudioEngine::loadASIOConfig()
{
    ASIOConfig cfg;
    if (auto xml = juce::XmlDocument::parse(getASIOConfigFile()))
    {
        if (xml->hasTagName("ASIOConfig"))
        {
            cfg.driverName = xml->getStringAttribute("driver");
            cfg.sampleRate = xml->getDoubleAttribute("sampleRate", 0.0);
            cfg.bufferSize = xml->getIntAttribute("bufferSize", 0);
            cfg.valid      = cfg.driverName.isNotEmpty();
        }
    }
    return cfg;
}

bool AudioEngine::saveASIOConfig()
{
    auto* jdm = getJuceDeviceManager();
    auto* dev = jdm != nullptr ? jdm->getCurrentAudioDevice() : nullptr;
    if (dev == nullptr)
        return false;

    juce::XmlElement root("ASIOConfig");
    root.setAttribute("driver",     dev->getName());
    root.setAttribute("sampleRate", dev->getCurrentSampleRate());
    root.setAttribute("bufferSize", dev->getCurrentBufferSizeSamples());

    const auto file = getASIOConfigFile();
    file.create();
    return root.writeTo(file);
}

//==============================================================================
// applyASIOConfig - mở driver / rate / buffer / channels (CHUẨN TRACKTION)
bool AudioEngine::applyASIOConfig(const ASIOConfig& cfg, bool forceReopen)
{
    if (engine == nullptr || ! cfg.valid)          // not gated on 'ready': see openSavedASIO()
        return false;

    auto* jdm = getJuceDeviceManager();
    if (jdm == nullptr)
        return false;

    // Tìm ASIO device type của driver đã lưu
    juce::String asioTypeName;
    for (auto* t : jdm->getAvailableDeviceTypes())
    {
        if (t == nullptr || ! t->getTypeName().toLowerCase().contains("asio"))
            continue;
        t->scanForDevices();
        if (t->getDeviceNames(false).contains(cfg.driverName))
        {
            asioTypeName = t->getTypeName();
            break;
        }
    }

    if (asioTypeName.isEmpty())
    {
        ptl::diag::write("[ASIO] saved driver '" + cfg.driverName + "' is no longer available");
        return false;
    }

    // Channels = exactly what user ticked last time
    auto& vis = ptl::ChannelVisibility::get();
    if (cfg.hasChannels)
        vis.set(cfg.driverName, cfg.inputs, cfg.outputs);
    else
        vis.initialise(cfg.driverName);

    const auto baseInCh = vis.getInputs(cfg.driverName);

    // Thêm input channels từ track routing đã lưu (tránh restart device sau này)
    juce::BigInteger trackNeeded;
    if (edit != nullptr && ! edit->isLoading())
    {
        const juce::Identifier trackInFirstId("ptlInFirst");
        const juce::Identifier trackInNoneId("ptlInNone");

        for (auto* t : te::getAudioTracks(*edit))
        {
            if (t == nullptr) continue;
            if (! t->state.hasProperty(trackInFirstId)
                || static_cast<bool>(t->state.getProperty(trackInNoneId, false)))
                continue;
            const int first = static_cast<int>(t->state.getProperty(trackInFirstId));
            if (first < 0) continue;

            int num = 1;
            if (auto* at = dynamic_cast<te::AudioTrack*>(t))
                num = (at->getMaxNumOfInputs() >= 2) ? 2 : 1;
            for (int i = 0; i < num; ++i)
                trackNeeded.setBit(first + i);
        }
    }

    const auto inCh  = makeStreamChannels(baseInCh | trackNeeded, true);
    const auto outCh = makeStreamChannels(vis.getOutputs(cfg.driverName), false);

    // NOTE: the field logs proved the LATE-MIC bug with virtual drivers (ASIO Link Pro) is caused
    // by closing+reopening the device (see the `alreadyOpen` handling below), NOT by the rate. A
    // virtual driver is therefore opened with the SAVED rate/buffer like any other driver; the fix
    // is purely "do not close and reopen a virtual driver".
    const double rate   = cfg.sampleRate > 0.0 ? cfg.sampleRate : 0.0;
    const int    buffer = 0;   // driver's own preferred size

    const bool isVirtual = looksLikeVirtualASIODriver (cfg.driverName);

    // The start-up seed (see seedStartupDeviceSetup) already opened exactly this driver with exactly these
    // channels: do not close and reopen it (an ASIO open costs ~1 s and can freeze the UI).
    // EXCEPTION: a virtual driver must ALWAYS be reopened here so it runs at its host rate/buffer
    // (the seed may have opened it with a forced rate from the saved config, which is what causes
    // the late mic). Skipping the reopen for a virtual driver would leave that extra latency in place.
    bool alreadyOpen = false;
    {
        auto* cur = jdm->getCurrentAudioDevice();
        alreadyOpen = cur != nullptr && cur->isOpen()
                      && jdm->getCurrentAudioDeviceType() == asioTypeName
                      && cur->getName() == cfg.driverName
                      && (rate <= 0.0 || std::abs (cur->getCurrentSampleRate() - rate) < 1.0)
                      && cur->getActiveInputChannels()  == makeStreamChannels (baseInCh | trackNeeded, true)
                      && cur->getActiveOutputChannels() == makeStreamChannels (vis.getOutputs (cfg.driverName), false);
    }

    // A VIRTUAL ASIO driver (ASIO Link Pro, Voicemeeter, VB-Audio...) owns its own internal
    // routing engine. Closing it and reopening (a second ASIO open) breaks that engine's sync and
    // the mic comes out LATE, even though the driver reports the SAME latency as before. Proof from
    // the field logs: when the start-up code KEPT the driver already opened by the seed
    // ("not reopened") the mic was fine; the moment the code closed+reopened it ("already open -
    // closing it" then re-initialise) the mic went late. So for a virtual driver we NEVER close and
    // reopen - we keep the device the seed already opened and only flush Tracktion's wave-device
    // rebuild against it. (A real hardware driver is unaffected either way, so this is safe.)
    if (isVirtual)
    {
        // Only keep it when the right driver is really open. (A blanket "already open" would skip
        // the initialise below and a first run would never open the device at all.)
        auto* cur = jdm->getCurrentAudioDevice();
        if (cur != nullptr && cur->isOpen()
            && jdm->getCurrentAudioDeviceType() == asioTypeName
            && cur->getName() == cfg.driverName)
        {
            alreadyOpen = true;
            ptl::diag::write("[ASIO] virtual driver '" + cfg.driverName
                             + "' is open from the seed - keeping it (a close+reopen breaks its "
                               "internal routing and makes the mic late)");
        }
    }

    // Forced reload (the one extra reload done right after start-up): even if the right driver is already open
    // with the right setup, close it and open it again - exactly what the manual reload does, which is what
    // removes the late mic on first launch.
    if (forceReopen && alreadyOpen)
    {
        alreadyOpen = false;
        ptl::diag::write("[ASIO] forced reload of '" + cfg.driverName + "' (one-time reload after start-up)");
    }

    // Đóng device cũ nếu đang mở
    if (! alreadyOpen)
    {
        auto* cur = jdm->getCurrentAudioDevice();
        ptl::diag::write(cur != nullptr
                             ? "[ASIO] before startup open: '" + cur->getName() + "' already open - closing it"
                             : juce::String("[ASIO] before startup open: no device open"));
        if (cur != nullptr)
        {
            SetupScope scope;
            jdm->closeAudioDevice();
        }
    }

    // Mở device với setup đã lưu
    juce::XmlElement state("DEVICESETUP");
    state.setAttribute("deviceType",            asioTypeName);
    state.setAttribute("audioInputDeviceName",  cfg.driverName);
    state.setAttribute("audioOutputDeviceName", cfg.driverName);
    state.setAttribute("audioDeviceRate",       rate);
    state.setAttribute("audioDeviceBufferSize", buffer);
    state.setAttribute("audioDeviceInChans",    inCh.toString(2));
    state.setAttribute("audioDeviceOutChans",   outCh.toString(2));

    juce::String err;
    if (! alreadyOpen)
    {
        SetupScope scope;
        err = jdm->initialise(2, 2, &state, false);   // <-- FIX #2: 2,2 thay vì 0,0
    }
    else
        ptl::diag::write("[ASIO] start-up: '" + cfg.driverName + "' is already open with this setup - not reopened");

    auto* dev = jdm->getCurrentAudioDevice();
    if (err.isNotEmpty() || dev == nullptr || ! dev->isOpen())
    {
        ptl::diag::write("[ASIO] restore failed: " + (err.isNotEmpty() ? err : juce::String("device not open")));
        return false;
    }

    {
        const auto cur = jdm->getAudioDeviceSetup();
        ptl::diag::write("[ASIO] startup channels: inCh=" + cur.inputChannels.toString(2)
                          + " outCh=" + cur.outputChannels.toString(2)
                          + " active in/out=" + juce::String(dev->getActiveInputChannels().countNumberOfSetBits())
                          + "/" + juce::String(dev->getActiveOutputChannels().countNumberOfSetBits())
                          + " inLat=" + juce::String(dev->getInputLatencyInSamples())
                          + " outLat=" + juce::String(dev->getOutputLatencyInSamples()));
    }

    {
        juce::String sizes;
        for (int b : dev->getAvailableBufferSizes())
            sizes << (sizes.isEmpty() ? "" : ",") << b;

        ptl::diag::write("[ASIO] latency check: buffer=" + juce::String(dev->getCurrentBufferSizeSamples())
                          + " (" + juce::String(dev->getCurrentBufferSizeSamples() * 1000.0 / juce::jmax(1.0, dev->getCurrentSampleRate()), 1)
                          + " ms) driverDefault=" + juce::String(dev->getDefaultBufferSize())
                          + " available=[" + sizes + "] inLat=" + juce::String(dev->getInputLatencyInSamples())
                          + " outLat=" + juce::String(dev->getOutputLatencyInSamples())
                          + " => round trip " + juce::String((dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples()) * 1000.0
                                                              / juce::jmax(1.0, dev->getCurrentSampleRate()), 1) + " ms");
    }

    ptl::diag::write("[ASIO] opened driver '" + cfg.driverName + "' rate="
                      + juce::String(dev->getCurrentSampleRate(), 0)
                      + " buffer=" + juce::String(dev->getCurrentBufferSizeSamples())
                      + " (saved rate=" + juce::String(cfg.sampleRate, 0)
                      + " buffer=" + juce::String(cfg.bufferSize) + ")");

    // === FIX #3: FLUSH TRACKTION'S ASYNC WAVE-DEVICE REBUILD ===
    // Tracktion rebuilds wave inputs/outputs asynchronously after the device change.
    // Không gọi dispatchPendingUpdates() → waveInputs/waveOutputs vẫn là list CŨ
    // → graph build với input sai → im tiếng cho đến khi double-click Devices
    auto& tdm = engine->getDeviceManager();
    tdm.dispatchPendingUpdates();
    ptl::diag::write("[ASIO] dispatchPendingUpdates() - wave device list flushed");

    // Rebuild playback graph against the (now correct) wave devices
    if (edit != nullptr && ! edit->isLoading())
    {
        edit->restartPlayback();
        ptl::diag::write("[ASIO] playback graph restarted after device open");
    }

    return true;
}

//==============================================================================
// Audio setup stored inside the .pt (driver / rate / buffer / channels), read from the file BEFORE any device opens.
void AudioEngine::readSessionAsioFromFile()
{
    sessionAsio = {};

    if (requestedFile == juce::File() || ! requestedFile.existsAsFile())
        return;

    // Same property names as kAsio*Id below (they are plain attributes of the root element of the .pt).
    if (auto xml = juce::XmlDocument::parse (requestedFile))
    {
        const auto driver = xml->getStringAttribute ("ptlAsioDriver");

        if (driver.isNotEmpty())
        {
            sessionAsio.valid       = true;
            sessionAsio.driverName  = driver;
            sessionAsio.sampleRate  = xml->getDoubleAttribute ("ptlAsioRate", 0.0);
            sessionAsio.bufferSize  = xml->getIntAttribute    ("ptlAsioBuffer", 0);
            sessionAsio.inputs .parseString (xml->getStringAttribute ("ptlAsioIn"),  16);
            sessionAsio.outputs.parseString (xml->getStringAttribute ("ptlAsioOut"), 16);
            sessionAsio.hasChannels = true;
            ptl::diag::write ("[ASIO] .pt asks for driver '" + driver + "' (read before the device is opened)");
        }
        else
            ptl::diag::write ("[ASIO] .pt holds no audio setup - using the last config");
    }
    else
        ptl::diag::write ("[ASIO] could not read the audio setup of the .pt (not plain XML?) - using the last config");
}

// Puts the driver we are going to use into Tracktion's "audio_device_setup", which dm.initialise() reads, so
// the first device opened is that driver (never a default one). Same XML as applyASIOConfig().
void AudioEngine::seedStartupDeviceSetup()
{
    auto* jdm = getJuceDeviceManager();
    if (engine == nullptr || jdm == nullptr)
        return;

    const auto cfg = sessionAsio.valid ? sessionAsio : loadASIOConfig();
    if (! cfg.valid)
        return;                                    // never configured: nothing to seed

    juce::String asioTypeName;
    for (auto* t : jdm->getAvailableDeviceTypes())
    {
        if (t == nullptr || ! t->getTypeName().toLowerCase().contains ("asio"))
            continue;
        t->scanForDevices();
        if (t->getDeviceNames (false).contains (cfg.driverName))
        {
            asioTypeName = t->getTypeName();
            break;
        }
    }

    if (asioTypeName.isEmpty())
        return;                                    // driver gone: applyASIOConfig() reports it later

    auto& vis = ptl::ChannelVisibility::get();
    if (cfg.hasChannels)
        vis.set (cfg.driverName, cfg.inputs, cfg.outputs);
    else
        vis.initialise (cfg.driverName);

    juce::XmlElement state ("DEVICESETUP");
    state.setAttribute ("deviceType",            asioTypeName);
    state.setAttribute ("audioInputDeviceName",  cfg.driverName);
    state.setAttribute ("audioOutputDeviceName", cfg.driverName);
    // A virtual ASIO driver (ASIO Link Pro...) must NEVER be opened with a rate saved from an
    // earlier session: it runs at its host rate and silently resamples otherwise (late mic).
    // rate = 0 = let the driver use its own current rate, exactly like a manual reload.
    const bool isVirtual = looksLikeVirtualASIODriver (cfg.driverName);
    state.setAttribute ("audioDeviceRate",       (! isVirtual && cfg.sampleRate > 0.0) ? cfg.sampleRate : 0.0);
    state.setAttribute ("audioDeviceBufferSize", 0);
    state.setAttribute ("audioDeviceInChans",    makeStreamChannels (vis.getInputs  (cfg.driverName), true ).toString (2));
    state.setAttribute ("audioDeviceOutChans",   makeStreamChannels (vis.getOutputs (cfg.driverName), false).toString (2));

    engine->getPropertyStorage().setXmlProperty (te::SettingID::audio_device_setup, state);
    ptl::diag::write ("[ASIO] start-up setup seeded: '" + cfg.driverName + "' (no default device is opened)");
}

//==============================================================================
// createOrLoadEdit - mở .pt hoặc tạo session mặc định
void AudioEngine::createOrLoadEdit()
{
    if (engine == nullptr)
        return;

    // Bước 0: sessionAsio (audio setup stored in the .pt) was already read in initialize(), before the device opened.

    // Bước 1: mở device TRƯỚC khi load .pt
    // (Tracktion rebuild wave devices tại đây → sẵn sàng cho graph)
    if (! openSavedASIO())
        ptl::diag::write ("[ASIO] startup open failed - the saved driver could not be opened");

    // Bước 2: load .pt
    if (requestedFile != juce::File())
    {
        if (requestedFile.existsAsFile())
            edit = te::loadEditFromFile(*engine, requestedFile);

        if (edit != nullptr)
        {
            currentSessionFile = requestedFile;
            readAudioSettingsFromEdit();
            ptl::diag::write("[session] opened " + requestedFile.getFullPathName());
        }
        else
        {
            requestedFileFailed = true;
            ptl::diag::write("[session] could NOT open " + requestedFile.getFullPathName());
        }
    }

    // Bước 3: default session
    if (edit == nullptr)
    {
        edit = te::createEmptyEdit(*engine, juce::File());
        if (edit != nullptr)
        {
            for (auto* t : te::getAudioTracks(*edit))
                edit->deleteTrack(t);
            edit->ensureMasterTrack();
            if (te::VolumeAndPanPlugin::Ptr mvp = edit->getMasterVolumePlugin())
                mvp->setVolumeDb(0.0f);
        }
    }

    if (edit != nullptr)
    {
        edit->setLatencyCompensationEnabled(false);
        pdcEnabledFlag.store(false);
        edit->ensureMasterTrack();
        edit->getTransport().ensureContextAllocated();
    }
}

//==============================================================================
// Session audio settings (.pt embedded)
namespace
{
    const juce::Identifier kAsioDriverId("ptlAsioDriver");
    const juce::Identifier kAsioRateId  ("ptlAsioRate");
    const juce::Identifier kAsioBufferId("ptlAsioBuffer");
    const juce::Identifier kAsioInId    ("ptlAsioIn");
    const juce::Identifier kAsioOutId   ("ptlAsioOut");
}

AudioEngine::ASIOConfig AudioEngine::captureASIOConfig()
{
    ASIOConfig cfg;
    auto* jdm = getJuceDeviceManager();
    auto* dev = jdm != nullptr ? jdm->getCurrentAudioDevice() : nullptr;

    if (dev == nullptr || ! jdm->getCurrentAudioDeviceType().containsIgnoreCase("asio"))
        return cfg;

    cfg.valid       = true;
    cfg.driverName  = dev->getName();
    cfg.sampleRate  = dev->getCurrentSampleRate();
    cfg.bufferSize  = dev->getCurrentBufferSizeSamples();

    auto& vis = ptl::ChannelVisibility::get();
    vis.initialise(cfg.driverName);
    cfg.inputs      = vis.getInputs(cfg.driverName);
    cfg.outputs     = vis.getOutputs(cfg.driverName);
    cfg.hasChannels = true;
    return cfg;
}

void AudioEngine::readAudioSettingsFromEdit()
{
    sessionAsio = {};
    if (edit == nullptr)
        return;

    const auto& st = edit->state;
    const auto driver = st.getProperty(kAsioDriverId).toString();
    if (driver.isEmpty())
        return;

    sessionAsio.valid       = true;
    sessionAsio.driverName  = driver;
    sessionAsio.sampleRate  = (double) st.getProperty(kAsioRateId);
    sessionAsio.bufferSize  = (int)    st.getProperty(kAsioBufferId);
    sessionAsio.inputs.parseString (st.getProperty(kAsioInId) .toString(), 16);
    sessionAsio.outputs.parseString(st.getProperty(kAsioOutId).toString(), 16);
    sessionAsio.hasChannels = true;
}

void AudioEngine::storeAudioSettingsInEdit()
{
    if (edit == nullptr)
        return;

    const auto cfg = captureASIOConfig();
    if (! cfg.valid)
        return;

    auto& st = edit->state;
    st.setProperty(kAsioDriverId, cfg.driverName,            nullptr);
    st.setProperty(kAsioRateId,   cfg.sampleRate,            nullptr);
    st.setProperty(kAsioBufferId, cfg.bufferSize,            nullptr);
    st.setProperty(kAsioInId,     cfg.inputs .toString(16),  nullptr);
    st.setProperty(kAsioOutId,    cfg.outputs.toString(16),  nullptr);
}

juce::String AudioEngine::audioSnapshot()
{
    const auto c = captureASIOConfig();
    if (! c.valid)
        return {};
    return c.driverName + "|" + juce::String(c.sampleRate, 0) + "|" + juce::String(c.bufferSize)
         + "|" + c.inputs.toString(16) + "|" + c.outputs.toString(16);
}

bool AudioEngine::hasUnsavedChanges()
{
    if (edit == nullptr)
        return false;
    if (edit->hasChangedSinceSaved())
        return true;

    const auto now = audioSnapshot();
    return now.isNotEmpty() && savedAudioSnapshot.isNotEmpty() && now != savedAudioSnapshot;
}

void AudioEngine::markSessionClean()
{
    if (edit != nullptr)
        edit->resetChangedStatus();
    savedAudioSnapshot = audioSnapshot();
}

bool AudioEngine::saveSessionTo(const juce::File& requested)
{
    if (edit == nullptr || requested == juce::File())
        return false;

    const auto target = requested.hasFileExtension(sessionExtension)
                            ? requested
                            : requested.getSiblingFile(requested.getFileName() + sessionExtension);

    if (! target.getParentDirectory().createDirectory().wasOk())
    {
        ptl::diag::write("[session] cannot create folder for " + target.getFullPathName());
        return false;
    }

    storeAudioSettingsInEdit();

    te::EditFileOperations ops(*edit);
    const auto tmp = target.getSiblingFile(target.getFileName() + ".tmp");
    tmp.deleteFile();

    if (! ops.writeToFile(tmp, false))
    {
        tmp.deleteFile();
        ptl::diag::write("[session] write failed: " + target.getFullPathName());
        return false;
    }

    if (! tmp.replaceFileIn(target) || target.getSize() <= 0)
    {
        tmp.deleteFile();
        ptl::diag::write("[session] could not replace: " + target.getFullPathName());
        return false;
    }

    currentSessionFile = target;
    edit->editFileRetriever = [target] { return target; };
    markSessionClean();

    ptl::diag::write("[session] saved " + target.getFullPathName());
    return true;
}

//==============================================================================
// Danh sách devices
juce::Array<juce::String> AudioEngine::listAvailableASIODevices() const
{
    juce::Array<juce::String> devices;
    auto* dm = const_cast<AudioEngine*>(this)->getDeviceManager();
    if (dm == nullptr)
        return devices;

    for (int i = 0; i < dm->getNumWaveOutDevices(); ++i)
        if (auto* dev = dm->getWaveOutDevice(i))
            devices.add(dev->getName());

    return devices;
}

juce::Array<juce::String> AudioEngine::listAvailableASIOInputs() const
{
    juce::Array<juce::String> devices;
    auto* dm = const_cast<AudioEngine*>(this)->getDeviceManager();
    if (dm == nullptr)
        return devices;

    for (int i = 0; i < dm->getNumWaveInDevices(); ++i)
        if (auto* dev = dm->getWaveInDevice(i))
            devices.add(dev->getName());

    return devices;
}

juce::Array<juce::String> AudioEngine::getDefaultVST3Paths()
{
    juce::Array<juce::String> paths;
    const auto programFiles = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory);

    paths.add(programFiles.getChildFile("Common Files").getChildFile("VST3").getFullPathName());
    paths.add(programFiles.getChildFile("VST3").getFullPathName());

    const auto localAppData = juce::File::getSpecialLocation(juce::File::windowsLocalAppData);
    paths.add(localAppData.getChildFile("Programs").getChildFile("Common")
                   .getChildFile("VST3").getFullPathName());
    return paths;
}

//==============================================================================
// Playback context / tap
void AudioEngine::playbackContextChanged()
{
    if (edit == nullptr || masterRecorder == nullptr || edit->getCurrentPlaybackContext() == nullptr)
        return;

    if (masterRecorder->isRecording()
        && std::abs(getSampleRate() - masterRecorder->getRecordingSampleRate()) > 0.5)
    {
        masterRecorder->stopRecording();
        ptl::diag::write("[REC] sample rate changed during recording -> recording stopped");
    }

    masterRecorder->setTapPrepared(false);
    tapWaitCount = 0;
    tapUnattachedLogged = false;

    auto& ctx = *edit->getCurrentPlaybackContext();

    ctx.setInsertOptionalLastStageNodeForDeviceCallback(
        [this] (te::OutputDevice& outputDevice, const te::CreateNodeParams&, std::unique_ptr<tracktion::graph::Node> node) -> std::unique_ptr<tracktion::graph::Node>
        {
            if (dynamic_cast<te::WaveOutputDevice*>(&outputDevice) == nullptr)
                return node;

            node = std::unique_ptr<tracktion::graph::Node>(new OutputGuardNode(std::move(node)));

            auto* rec = getMasterRecorder();
            if (rec == nullptr)
                return node;

            auto& dm = edit->engine.getDeviceManager();
            // The tap sits on the output that is really enabled (= the one playing to the speakers):
            // the default wave out if it is enabled, otherwise the first enabled output.
            const auto defaultID = dm.getDefaultWaveOutDeviceID();
            juce::String tapID, firstEnabledID;

            for (auto* d : dm.getWaveOutputDevices())
            {
                if (d == nullptr || ! d->isEnabled())
                    continue;

                if (firstEnabledID.isEmpty())
                    firstEnabledID = d->getDeviceID();

                if (defaultID.isNotEmpty() && d->getDeviceID() == defaultID)
                {
                    tapID = defaultID;
                    break;
                }
            }

            if (tapID.isEmpty())
                tapID = firstEnabledID;

            if (tapID != outputDevice.getDeviceID())
                return node;

            return std::unique_ptr<tracktion::graph::Node>(new TapNode(rec, std::move(node)));
        });

    juce::WeakReference<AudioEngine> weak(this);
    juce::Timer::callAfterDelay(150, [weak]
    {
        if (auto* self = weak.get())
            self->ensureTapActive(true);
    });
}

bool AudioEngine::isReadyForTapRetry() const
{
    auto* self = const_cast<AudioEngine*>(this);

    if (edit == nullptr || edit->isLoading())
        return false;

    // The ONE automatic rebuild (tap) runs during start-up, while the veil is still up and play() has not been
    // called yet: nothing is audible, so it cannot cut any sound. Only the later checks wait for the end of start-up.
    if (tapAutoRestartDone && isStartupPhase())
        return false;

    auto* jdm = self->getJuceDeviceManager();
    auto* dev = jdm != nullptr ? jdm->getCurrentAudioDevice() : nullptr;

    return dev != nullptr && dev->isOpen() && dev->isPlaying();
}

void AudioEngine::ensureTapActive(bool externalChange)
{
    juce::ignoreUnused(externalChange);   // kept for the callers; no retry counter any more

    if (edit == nullptr || masterRecorder == nullptr)
    {
        startupGraphSettled = true;
        return;
    }

    if (masterRecorder->isTapPrepared())
    {
        tapWaitCount = 0;
        startupGraphSettled = true;
        return;
    }

    juce::WeakReference<AudioEngine> weak(this);

    if (! isReadyForTapRetry())
    {
        if (! tapPollPending && tapWaitCount < 60)
        {
            ++tapWaitCount;
            tapPollPending = true;
            juce::Timer::callAfterDelay(500, [weak]
            {
                if (auto* self = weak.get())
                {
                    self->tapPollPending = false;
                    self->ensureTapActive(true);
                }
            });
        }
        return;
    }

    tapWaitCount = 0;

    // Only ONE automatic restart per app run (the first graph is built before the tap callback
    // exists, so it needs one rebuild). After that NEVER restart on its own: a restart cuts the
    // sound. If the tap is still missing, just log it; MasterRecorder::beginRecording() reports it.
    if (tapAutoRestartDone)
    {
        if (! tapUnattachedLogged)
        {
            tapUnattachedLogged = true;
            ptl::diag::write("[REC] master tap could not be attached to the audio graph (no automatic restart)");
        }
        return;
    }

    tapAutoRestartDone = true;
    ptl::diag::write("[REC] tap not in graph yet -> restarting playback (once, at startup)");
    edit->restartPlayback();

    juce::Timer::callAfterDelay(300, [weak]
    {
        if (auto* self = weak.get())
        {
            self->ensureTapActive(false);
            self->startupGraphSettled = true;      // the start-up rebuild is over: MainWindow may start playing now
        }
    });
}

//==============================================================================
// isAudioEngineInStartupPhase (declared in MasterRecorder.h)
namespace ptl {
bool isAudioEngineInStartupPhase()
{
    return startupPhaseFlag.load(std::memory_order_relaxed);
}
} // namespace ptl
