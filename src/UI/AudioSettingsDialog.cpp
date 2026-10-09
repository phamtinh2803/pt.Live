/*
    pt.Live - Live Singing DAW
    Audio Settings dialog — Cubase / Studio One style.

    Only ASIO drivers are listed. Selecting a driver opens it; only then are
    its input/output channels queried and shown as checkboxes.
*/

#include "include/UI/AudioSettingsDialog.h"
#include "include/Audio/PtLog.h"
#include "include/Audio/ChannelVisibility.h"

using namespace ptl;

namespace
{
    // "1,2,11,12" (1-based) - which physical channels the driver really has active.
    juce::String listChannels (const juce::BigInteger& bits)
    {
        juce::StringArray out;
        for (int i = bits.findNextSetBit (0); i >= 0; i = bits.findNextSetBit (i + 1))
            out.add (juce::String (i + 1));
        return out.joinIntoString (",");
    }
}

//==============================================================================
AudioSettingsContent::AudioSettingsContent (AudioEngine& engine, std::function<void (bool)> onPDCChanged)
    : audioEngine (engine), pdcChanged (std::move (onPDCChanged))
{
    setOpaque (true);

    for (auto* l : { &driverCaption, &rateCaption, &bufferCaption, &latencyCaption,
                     &inCaption, &outCaption })
    {
        l->setColour (juce::Label::textColourId, theme::textDimmed);
        l->setFont (juce::Font (juce::FontOptions (11.0f)));
        l->setJustificationType (juce::Justification::centredLeft);
    }

    latencyValue.setColour (juce::Label::textColourId, theme::textPrimary);
    latencyValue.setFont (juce::Font (juce::FontOptions (12.0f)));
    latencyValue.setJustificationType (juce::Justification::centredLeft);

    driverBox.onChange = [this] { onDriverChanged(); };
    rateBox.onChange   = [this] { onRateChanged(); };
    bufferBox.onChange = [this] { onBufferChanged(); };

    panelBtn.onClick = [this]
    {
        if (auto* dev = devMgr().getCurrentAudioDevice())
            if (dev->hasControlPanel())
            {
                dev->showControlPanel();
                panelOpened = true;           // followed when the app is brought back to the front
            }
    };
    // PDC: ON = the engine delays every other path so all tracks stay time-aligned (needed for per-track
    // recording); OFF = no compensation at all, lowest live latency. OFF until the user ticks it.
    pdcToggle.setToggleState (AudioEngine::isPDCEnabled(), juce::dontSendNotification);
    pdcToggle.setColour (juce::ToggleButton::textColourId, theme::textPrimary);
    pdcToggle.setColour (juce::ToggleButton::tickColourId, theme::accent);
    pdcToggle.setTooltip ("Plugin Delay Compensation: aligns all tracks (needed for per-track recording). "
                          "Off = no compensation at all, lowest live latency.");
    pdcToggle.onClick = [this]
    {
        const bool on = pdcToggle.getToggleState();
        if (pdcChanged)
            pdcChanged (on);                       // main window: engine + refresh of heavy-plugin flags
        else
            audioEngine.setPDCEnabled (on);
    };
    closeBtn.onClick = [this]
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
    };

    // NOTE: inHolder / outHolder are children of their Viewport (setViewedComponent
    // reparents them). They must NOT be passed to addAndMakeVisible() here,
    // otherwise they get stolen from the viewport and drawn over the dialog.
    inViewport.setViewedComponent (&inHolder, false);
    inViewport.setScrollBarsShown (true, false);
    outViewport.setViewedComponent (&outHolder, false);
    outViewport.setScrollBarsShown (true, false);

    addAndMakeVisible (&driverCaption);
    addAndMakeVisible (&driverBox);
    addAndMakeVisible (&rateCaption);
    addAndMakeVisible (&rateBox);
    addAndMakeVisible (&bufferCaption);
    addAndMakeVisible (&bufferBox);
    addAndMakeVisible (&latencyCaption);
    addAndMakeVisible (&latencyValue);
    addAndMakeVisible (&panelBtn);
    addAndMakeVisible (&pdcToggle);
    addAndMakeVisible (&closeBtn);
    addAndMakeVisible (&inCaption);
    addAndMakeVisible (&outCaption);
    addAndMakeVisible (&inViewport);
    addAndMakeVisible (&outViewport);

    devMgr().addChangeListener (this);
    audioEngine.getDeviceConfigBroadcaster().addChangeListener (this);

    rebuildDriverList (true);
    rebuildRateList();
    rebuildBufferList();
    rebuildChannelRows();
    updateLatencyLabel();
    resized();
    startTimer (500);
}

AudioSettingsContent::~AudioSettingsContent()
{
    // No Save button: the device setup (driver / sample rate / buffer size) is stored whenever the dialog closes.
    audioEngine.saveASIOConfig();

    stopTimer();
    audioEngine.getDeviceConfigBroadcaster().removeChangeListener (this);
    devMgr().removeChangeListener (this);
}

juce::AudioDeviceManager& AudioSettingsContent::devMgr() const
{
    return *audioEngine.getJuceDeviceManager();
}

//==============================================================================
void AudioSettingsContent::rebuildDriverList (bool rescan)
{
    driverBox.clear (juce::dontSendNotification);
    driverOwnerTypes.clear();

    // NEVER switch the current device type here: setCurrentAudioDeviceType() opens the new type's DEFAULT device,
    // and for ASIO that is ASIO4ALL (it popped up every time this list was rebuilt while another type was
    // current). Listing needs no switch: each ASIO type can be scanned and asked for its names while it is not current.
    for (auto* type : devMgr().getAvailableDeviceTypes())
    {
        if (type == nullptr)
            continue;
        if (! type->getTypeName().toLowerCase().contains ("asio"))
            continue;

        if (rescan)
            type->scanForDevices();

        auto names = type->getDeviceNames (false);   // (a type that was never scanned: scanned by the line above at dialog start)
        for (const auto& n : names)
        {
            driverBox.addItem (n, driverBox.getNumItems() + 1);
            driverOwnerTypes.add (type->getTypeName());
        }
    }

    const juce::String active = devMgr().getCurrentAudioDevice() != nullptr
                                    ? devMgr().getCurrentAudioDevice()->getName()
                                    : juce::String();
    for (int i = 0; i < driverBox.getNumItems(); ++i)
        if (driverBox.getItemText (i) == active)
            driverBox.setSelectedItemIndex (i, juce::dontSendNotification);

    if (driverBox.getNumItems() == 0)
        driverBox.setText ("No ASIO drivers found", juce::dontSendNotification);
}

void AudioSettingsContent::rebuildRateList()
{
    rateBox.clear (juce::dontSendNotification);
    shownRates.clear();

    const auto cfg = audioEngine.getDeviceConfig();
    int id = 0;
    for (double r : cfg.sampleRates)
    {
        shownRates.add (r);
        rateBox.addItem (r >= 1000.0 ? juce::String (r / 1000.0, 1) + " kHz"
                                     : juce::String (r, 0) + " Hz", ++id);
        if (std::abs (r - cfg.sampleRate) < 1.0)
            rateBox.setSelectedId (id, juce::dontSendNotification);
    }
    rateBox.setEnabled (id > 0);
}
void AudioSettingsContent::rebuildBufferList()
{
    bufferBox.clear (juce::dontSendNotification);
    shownBuffers.clear();

    const auto cfg = audioEngine.getDeviceConfig();
    int id = 0;
    for (int sz : cfg.bufferSizes)
    {
        shownBuffers.add (sz);
        juce::String label = juce::String (sz) + " samples";
        if (cfg.sampleRate > 0.0)
            label << "  (" << juce::String (sz * 1000.0 / cfg.sampleRate, 1) << " ms)";
        bufferBox.addItem (label, ++id);
        if (sz == cfg.bufferSize)
            bufferBox.setSelectedId (id, juce::dontSendNotification);
    }

    // A driver such as ASIO Link Pro offers ONE size (its host buffer): it is set in the driver's
    // own control panel and the app follows it.
    const bool fixedSize = shownBuffers.size() <= 1;
    bufferBox.setEnabled (! fixedSize);
    bufferBox.setTooltip (fixedSize ? "This driver has a fixed buffer size. Change it in the driver's "
                                      "Control Panel - the app follows automatically."
                                    : juce::String());
}
void AudioSettingsContent::updateLatencyLabel()
{
    const auto cfg = audioEngine.getDeviceConfig();
    if (! cfg.open || cfg.sampleRate <= 0.0)
    {
        latencyValue.setText ("-", juce::dontSendNotification);
        return;
    }

    const double inMs  = cfg.inputLatency  * 1000.0 / cfg.sampleRate;
    const double outMs = cfg.outputLatency * 1000.0 / cfg.sampleRate;

    latencyValue.setText ("In " + juce::String (inMs, 1) + " ms   /   Out "
                              + juce::String (outMs, 1) + " ms",
                          juce::dontSendNotification);
}
void AudioSettingsContent::rebuildChannelRows()
{
    inputRows.clear ();
    outputRows.clear ();
    inHolder.removeAllChildren ();
    outHolder.removeAllChildren ();

    auto* dev = devMgr().getCurrentAudioDevice();
    if (dev == nullptr)
    {
        inHolder.setSize (1, 1);
        outHolder.setSize (1, 1);
        return;
    }

    const auto inNames  = dev->getInputChannelNames();
    const auto outNames = dev->getOutputChannelNames();
    // Ticks = what the user chose (ChannelVisibility), not what the driver happens to stream.
    auto& vis = ChannelVisibility::get();
    const auto drv = dev->getName();
    vis.initialise (drv);
    const auto actIn  = vis.getInputs (drv);
    const auto actOut = vis.getOutputs (drv);

    for (int ch = 0; ch < inNames.size(); ++ch)
    {
        auto* row = new ChannelRow ("IN  " + inNames[ch], actIn[ch]);
        row->onToggle = [this, ch] (bool on) { toggleChannels (true, ch, 1, on); };
        inputRows.add (row);
        inHolder.addAndMakeVisible (row);
    }

    for (int ch = 0; ch < outNames.size(); ch += 2)
    {
        const bool hasRight = (ch + 1) < outNames.size();
        const juce::String pair = "OUT  " + outNames[ch]
            + (hasRight ? juce::String (" / ") + outNames[ch + 1] : juce::String());
        const bool on = actOut[ch] || (hasRight && actOut[ch + 1]);

        auto* row = new ChannelRow (pair, on);
        const int base = ch;
        row->onToggle = [this, base, hasRight] (bool on)
        {
            toggleChannels (false, base, hasRight ? 2 : 1, on);
        };
        outputRows.add (row);
        outHolder.addAndMakeVisible (row);
    }

    layoutChannelRows();
}

//==============================================================================
void AudioSettingsContent::onDriverChanged()
{
    const int idx = driverBox.getSelectedItemIndex();
    if (idx < 0 || idx >= driverOwnerTypes.size())
        return;

    const juce::String driver = driverBox.getItemText (idx);
    const juce::String owner  = driverOwnerTypes[idx];

    AudioEngine::SetupScope scope;      // everything below is app-made, not a driver reset

    // Same driver picked again: nothing to do.
    if (owner == devMgr().getCurrentAudioDeviceType()
        && driver == devMgr().getAudioDeviceSetup().outputDeviceName)
        return;

    // Unload the current driver completely BEFORE the next one is loaded (e.g. ASIO Link Pro ->
    // Tascam 102i), so the old driver releases the hardware / its virtual routing and the new one
    // starts clean and reports its own buffer size and sample rate.
    if (owner != devMgr().getCurrentAudioDeviceType())
    {
        // Other device type (e.g. WASAPI -> ASIO): open the chosen driver directly. Going through
        // setCurrentAudioDeviceType() would open that type's default device first = ASIO4ALL.
        const auto e = audioEngine.openDriverExact (owner, driver);

        if (e.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Audio Settings", e, "OK");
            return;
        }

        audioEngine.saveASIOConfig();
        refreshFromEngine();
        return;
    }

    devMgr().closeAudioDevice();

    auto setup = devMgr().getAudioDeviceSetup();

    setup.outputDeviceName = driver;
    setup.inputDeviceName  = driver;
    // Default is In 1-2 / Out 1-2 only; a driver seen before gets the ticks the user chose last time.
    // The driver is opened with exactly the ticked channels (see AudioEngine::makeStreamChannels);
    // with nothing ticked it still opens with Out 1/2 so its sample rate and buffer size can be read.
    ChannelVisibility::get().initialise (driver);
    setup.useDefaultInputChannels  = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels  = audioEngine.makeStreamChannels (ChannelVisibility::get().getInputs  (driver), true);
    setup.outputChannels = audioEngine.makeStreamChannels (ChannelVisibility::get().getOutputs (driver), false);

    // Let the NEW driver choose its own preferred buffer size (what Cubase does). Without this
    // the value used with the previous driver is carried over; a virtual driver such as
    // ASIO Link Pro then runs at a size different from its host buffer and silently adds an
    // extra buffering stage that the driver never reports (so the latency label looks the same).
    setup.bufferSize = 0;
    setup.sampleRate = 0.0;           // the new driver's own current rate, not the previous driver's

    auto err = devMgr().setAudioDeviceSetup (setup, true);
    if (err.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Audio Settings", err, "OK");
        return;
    }

    // FIX: Flush Tracktion's async wave-device rebuild NOW (chuẩn Tracktion Engine).
    // Không có bước này, waveInputs/waveOutputs là list cũ → track combos sai → im tiếng.
    if (auto* dm = audioEngine.getDeviceManager())
        dm->dispatchPendingUpdates();

    // One open of the chosen driver is all it takes (like Cubase): no reload afterwards.
    audioEngine.saveASIOConfig();         // remember it, the next start opens this driver again
    refreshFromEngine();
}

void AudioSettingsContent::onRateChanged()
{
    const int idx = rateBox.getSelectedId() - 1;
    if (! juce::isPositiveAndBelow (idx, shownRates.size()))
        return;

    const double wanted = shownRates[idx];
    if (std::abs (wanted - audioEngine.getDeviceConfig().sampleRate) < 1.0)
        return;

    // Pushed to the driver, then the rate it REALLY runs at is read back.
    if (! audioEngine.requestSampleRate (wanted))
    {
        rebuildRateList();            // shows the real value again
        rebuildBufferList();
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Audio Settings",
            "The driver did not accept " + juce::String (wanted / 1000.0, 1) + " kHz.\n"
            "Change the sample rate in the driver's Control Panel; the app will follow.", "OK");
        return;
    }

    // Tracktion rebuilds the playback graph itself on device restart (prepareToStart). The
    // dispatchPendingUpdates() inside requestSampleRate() ensures the wave-device list is fresh.
    refreshFromEngine();
}
void AudioSettingsContent::onBufferChanged()
{
    const int idx = bufferBox.getSelectedId() - 1;
    if (! juce::isPositiveAndBelow (idx, shownBuffers.size()))
        return;

    const int wanted = shownBuffers[idx];
    if (wanted == audioEngine.getDeviceConfig().bufferSize)
        return;

    if (! audioEngine.requestBufferSize (wanted))
    {
        rebuildBufferList();          // shows the real value again
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Audio Settings",
            "The driver did not accept a buffer of " + juce::String (wanted) + " samples.\n"
            "Change it in the driver's Control Panel; the app will follow.", "OK");
        return;
    }

    // Tracktion rebuilds the playback graph itself on device restart (prepareToStart). The
    // dispatchPendingUpdates() inside requestBufferSize() ensures the wave-device list is fresh.
    refreshFromEngine();
}
void AudioSettingsContent::toggleChannels (bool isInput, int firstChannel, int count, bool shouldBeOn)
{
    auto* dev = devMgr().getCurrentAudioDevice();
    if (dev == nullptr)
        return;

    auto& vis = ChannelVisibility::get();
    const auto drv = dev->getName();
    vis.initialise (drv);

    auto in  = vis.getInputs (drv);
    auto out = vis.getOutputs (drv);
    auto& bits = isInput ? in : out;
    for (int i = 0; i < count; ++i)
        bits.setBit (firstChannel + i, shouldBeOn);

    vis.set (drv, in, out);                // what the track combos offer

    // No driver reload. The device is restarted once, in place, only when the set of channels the driver
    // streams really changed (Tracktion rebuilds the playback graph by itself when the device restarts).
    audioEngine.applyChannelSelection();

    // The setup may be unchanged (=> no change message), so refresh the ticks ourselves. Deferred:
    // we are inside a ChannelRow's click handler and rebuilding deletes those rows.
    juce::Component::SafePointer<AudioSettingsContent> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->rebuildChannelRows();
    });
}

void AudioSettingsContent::refreshFromEngine()
{
    rebuildRateList();
    rebuildBufferList();
    updateLatencyLabel();
}
void AudioSettingsContent::refreshAfterPanel()
{
    refreshFromEngine();              // just show what the driver reports now, nothing is reloaded
}
void AudioSettingsContent::timerCallback()
{
    // Back in front after the driver's control panel was opened: follow the driver once so the
    // buffer size and sample rate chosen there show up. (Driver-requested resets are handled by
    // the engine itself, whether this dialog is open or not.)
    const bool fg = juce::Process::isForegroundProcess();
    if (panelOpened && fg && ! wasForeground)
    {
        panelOpened = false;
        wasForeground = fg;
        refreshAfterPanel();
        return;
    }
    wasForeground = fg;
}
void AudioSettingsContent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &audioEngine.getDeviceConfigBroadcaster())
    {
        refreshFromEngine();          // rate / buffer / latency moved (driver side or app side)
        return;
    }

    if (auto* dev = devMgr().getCurrentAudioDevice())
    {
        const auto setup = devMgr().getAudioDeviceSetup();
        ptl::diag::write ("[ASIO] driver=" + dev->getName()
                          + " | requestedBuf=" + juce::String (setup.bufferSize)
                          + " actualBuf=" + juce::String (dev->getCurrentBufferSizeSamples())
                          + " defaultBuf=" + juce::String (dev->getDefaultBufferSize())
                          + " | rate=" + juce::String (dev->getCurrentSampleRate(), 0)
                          + " | inLat=" + juce::String (dev->getInputLatencyInSamples())
                          + " outLat=" + juce::String (dev->getOutputLatencyInSamples())
                          + " samples | activeIn=" + juce::String (dev->getActiveInputChannels().countNumberOfSetBits())
                          + " activeOut=" + juce::String (dev->getActiveOutputChannels().countNumberOfSetBits())
                          + " | in[" + listChannels (dev->getActiveInputChannels()) + "]"
                          + " out[" + listChannels (dev->getActiveOutputChannels()) + "]");
    }

    rebuildDriverList (false);   // no rescan here: avoids re-entrant device changes
    rebuildRateList();
    rebuildBufferList();
    rebuildChannelRows();
    updateLatencyLabel();
    repaint ();
}
//==============================================================================
void AudioSettingsContent::layoutChannelRows()
{
    const int rowH = 26, gap = 4;
    const int scrollW = 14;

    int y = 0;
    const int iw = juce::jmax (inViewport.getWidth() - scrollW, 1);
    for (auto* r : inputRows)
    {
        r->setBounds (0, y, iw, rowH);
        y += rowH + gap;
    }
    inHolder.setSize (iw, juce::jmax (y, 20));

    y = 0;
    const int ow = juce::jmax (outViewport.getWidth() - scrollW, 1);
    for (auto* r : outputRows)
    {
        r->setBounds (0, y, ow, rowH);
        y += rowH + gap;
    }
    outHolder.setSize (ow, juce::jmax (y, 20));
}

void AudioSettingsContent::resized()
{
    auto area = getLocalBounds().reduced (16);

    const int captionH = 16;
    const int boxH     = 28;
    const int rowGap   = 10;

    //=== Footer (row 5): Close button, bottom-right ===
    {
        auto footer = area.removeFromBottom (40);
        closeBtn.setBounds (footer.removeFromRight (100).withTrimmedTop (8));
    }

    //=== Row 1: ASIO Driver (full width) ===
    {
        auto row = area.removeFromTop (captionH + boxH);
        driverCaption.setBounds (row.removeFromTop (captionH));
        driverBox.setBounds (row.removeFromTop (boxH));
    }
    area.removeFromTop (rowGap);

    //=== Row 2: Sample Rate | Buffer Size | Latency ===
    {
        auto row = area.removeFromTop (captionH + boxH);
        const int gap  = 12;
        const int colW = (row.getWidth() - gap * 2) / 3;

        auto r = row.removeFromLeft (colW);
        row.removeFromLeft (gap);
        auto b = row.removeFromLeft (colW);
        row.removeFromLeft (gap);
        auto l = row;

        rateCaption.setBounds (r.removeFromTop (captionH));
        rateBox.setBounds (r.removeFromTop (boxH));

        bufferCaption.setBounds (b.removeFromTop (captionH));
        bufferBox.setBounds (b.removeFromTop (boxH));

        latencyCaption.setBounds (l.removeFromTop (captionH));
        latencyValue.setBounds (l.removeFromTop (boxH));
    }
    area.removeFromTop (rowGap);

    //=== Row 3: Control Panel ===
    {
        auto row = area.removeFromTop (30);
        panelBtn.setBounds (row.removeFromLeft (140));
        row.removeFromLeft (16);
        pdcToggle.setBounds (row.removeFromLeft (150));
    }
    area.removeFromTop (rowGap + 2);

    //=== Row 4: Input channels (left) | Output channels (right) ===
    {
        auto chanArea = area;
        const int gap  = 16;
        const int halfW = (chanArea.getWidth() - gap) / 2;

        auto left  = chanArea.removeFromLeft (halfW);
        chanArea.removeFromLeft (gap);
        auto right = chanArea;

        inFrameRect = left;
        {
            auto col = left.reduced (10, 8);
            inCaption.setBounds (col.removeFromTop (captionH));
            col.removeFromTop (4);
            inViewport.setBounds (col);
        }

        outFrameRect = right;
        {
            auto col = right.reduced (10, 8);
            outCaption.setBounds (col.removeFromTop (captionH));
            col.removeFromTop (4);
            outViewport.setBounds (col);
        }
    }

    layoutChannelRows();
}

void AudioSettingsContent::paint (juce::Graphics& g)
{
    g.fillAll (theme::bgPrimary);

    // only draw frames if they have a sane size (after first resized())
    if (inFrameRect.getHeight() > 4 && outFrameRect.getHeight() > 4)
    {
        auto frame = [&] (const juce::Rectangle<int>& r)
        {
            g.setColour (theme::bgSecondary);
            g.fillRoundedRectangle (r.toFloat(), 8.0f);
            g.setColour (theme::borderLight);
            g.drawRoundedRectangle (r.toFloat(), 8.0f, 1.5f);
        };
        frame (inFrameRect);
        frame (outFrameRect);
    }
}

//==============================================================================
void AudioSettingsDialog::show (AudioEngine& engine, juce::Component* parent, std::function<void (bool)> onPDCChanged)
{
    auto* content = new AudioSettingsContent (engine, std::move (onPDCChanged));

    // Cap to the work area so the dialog still fits at high OS scale (200-300%) on small screens.
    juce::Rectangle<int> work (0, 0, 1920, 1080);
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    if (parent != nullptr)
        work = displays.getDisplayForRect (parent->getScreenBounds())->userArea;
    else if (auto* d = displays.getPrimaryDisplay())
        work = d->userArea;

    content->setSize (juce::jmin (640, juce::roundToInt (work.getWidth()  * 0.9f)),
                      juce::jmin (520, juce::roundToInt (work.getHeight() * 0.85f)));

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (content);
    opts.dialogTitle = "pt.Live - Audio Settings";
    opts.dialogBackgroundColour = theme::bgPrimary;
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;
    theme::launchDialog (opts, 520, 400);
}
