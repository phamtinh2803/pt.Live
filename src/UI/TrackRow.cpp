/*
    pt.Live - Live Singing DAW
    TrackRow implementation
*/

#include "include/Audio/PtLog.h"
#include "include/Audio/ChannelVisibility.h"
#include "include/UI/TrackRow.h"
#include "include/Audio/AudioEngine.h"
#include "include/Theme.h"

#include <limits>

using namespace ptl;

namespace
{
    ptl::icons::Id iconFor (TrackManager::TrackType k)
    {
        switch (k)
        {
            case TrackManager::TrackType::StereoIn:  return ptl::icons::Id::Music;
            case TrackManager::TrackType::MonoIn:    return ptl::icons::Id::Mic;
            case TrackManager::TrackType::FXChannel: return ptl::icons::Id::Zap;
            case TrackManager::TrackType::Master:    return ptl::icons::Id::Volume;
        }
        return ptl::icons::Id::Music;
    }

    // Muted tints: the glyph carries the meaning, colour is only a hint
    juce::Colour colourFor (TrackManager::TrackType k)
    {
        switch (k)
        {
            case TrackManager::TrackType::StereoIn:  return theme::success;
            case TrackManager::TrackType::MonoIn:    return theme::accent;
            case TrackManager::TrackType::FXChannel: return theme::warning;
            case TrackManager::TrackType::Master:    return theme::accentDeep;
        }
        return theme::accent;
    }

    void styleCombo (juce::ComboBox& c, float fontHeight)
    {
        c.setColour (juce::ComboBox::backgroundColourId, theme::raised);
        c.setColour (juce::ComboBox::outlineColourId,    theme::outline);
        c.setColour (juce::ComboBox::textColourId,       theme::text);
        c.setColour (juce::ComboBox::arrowColourId,      theme::textDim);
        c.setJustificationType (juce::Justification::centredLeft);
        c.getProperties().set ("fontHeight", (double) fontHeight);
    }

    // The user's choice is ALSO stored on the track itself (saved with the .pt). Tracktion's own routing data is
    // matched by device id when the file is opened, which fails when the wave device list is built differently
    // (mono / stereo grouping, driver not open yet) -> the track came back as "No Input".
    const juce::Identifier kInFirstId ("ptlInFirst");      // first physical input channel (0-based)
    const juce::Identifier kInNumId   ("ptlInNum");        // 1 (mono) or 2 (stereo)
    const juce::Identifier kInNoneId  ("ptlInNone");       // the user explicitly chose "No Input"
    const juce::Identifier kOutDevId  ("ptlOutDevice");    // Master: wave output device id chosen in the combo

    constexpr int kPadTop = 10, kCtrlH = 27, kMeterGap = 6, kMeterH = 5, kPadL = 9, kCardMargin = 2;
    constexpr int kIconW = 24, kBtn = 22, kBtnGap = 2, kComboW = 104, kChevronW = 14;
}

//==============================================================================
TrackRow::TrackRow(TrackManager& mgr, te::Track* trk)
    : trackManager(mgr),
      track(trk),
      kind(mgr.getTrackType(trk)),
      deleteButton(icons::Id::Trash,      ui::IconButton::Style::Delete),
      muteButton  (icons::Id::Volume2,    ui::IconButton::Style::Mute),
      soloButton  (icons::Id::Headphones, ui::IconButton::Style::Solo)
{
    const bool isMaster = (kind == TrackManager::TrackType::Master);
    const bool isFx     = (kind == TrackManager::TrackType::FXChannel);

    // NOT opaque: paint() only fills the rounded card (2px margin above/below + rounded corners).
    // With setOpaque(true) those pixels were never painted and showed up as a white outline.
    setOpaque(false);

    // Name -------------------------------------------------------------------
    nameLabel.setText(isMaster ? "MASTER" : track->getName(), juce::dontSendNotification);
    nameLabel.setFont(theme::font(theme::szLabel, true));
    nameLabel.setColour(juce::Label::textColourId, theme::text);
    nameLabel.setEditable(false, ! isMaster, false);          // double-click to rename
    nameLabel.onTextChange = [this]
    {
        auto n = nameLabel.getText().trim();
        if (n.isEmpty())
            n = track->getName();

        trackManager.renameTrack(track, n);
        nameLabel.setText(n, juce::dontSendNotification);

        if (onRenamed)
            onRenamed(track);
    };
    addAndMakeVisible(nameLabel);

    // Combos ------------------------------------------------------------------
    styleCombo(inputSelector,  theme::szBody);   // same font/size as the output combo
    styleCombo(outputSelector, theme::szBody);
    inputSelector.onChange  = [this] { handleInputSelectionChanged(); };
    outputSelector.onChange = [this] { handleOutputSelectionChanged(); };

    arrowLabel.setText(">", juce::dontSendNotification);
    arrowLabel.setFont(theme::font(theme::szSmall));
    arrowLabel.setJustificationType(juce::Justification::centred);
    arrowLabel.setColour(juce::Label::textColourId, theme::textFaint);
    arrowLabel.setInterceptsMouseClicks(false, false);

    if (! isMaster && ! isFx)
    {
        addAndMakeVisible(inputSelector);
        addAndMakeVisible(arrowLabel);
    }
    addAndMakeVisible(outputSelector);

    // Buttons -----------------------------------------------------------------
    deleteButton.onClick = [this] { if (onDeleteRequested) onDeleteRequested(track); };
    deleteButton.setEnabled(! isMaster);
    addAndMakeVisible(deleteButton);

    // Mute: audio tracks use the track's mute; the Master bus has no mute flag, so its
    // button pulls the master fader down and restores it on un-mute.
    muteButton.setClickingTogglesState(true);
    if (! isMaster)
        muteButton.setToggleState(track->isMuted(false), juce::dontSendNotification);
    muteButton.onClick = [this]
    {
        const bool mute = muteButton.getToggleState();

        if (kind == TrackManager::TrackType::Master)
        {
            if (auto* vp = trackManager.getVolumePlugin(track))
            {
                if (mute)
                {
                    masterMuteRestoreDb = vp->getVolumeDb();
                    vp->setVolumeDb(-100.0f);
                }
                else
                    vp->setVolumeDb(masterMuteRestoreDb);
            }
        }
        else
            track->setMute(mute);
    };
    addAndMakeVisible(muteButton);

    if (! isMaster)                                            // OutputBus: solo hidden
    {
        soloButton.setClickingTogglesState(true);
        soloButton.onClick = [this] { track->setSolo(soloButton.getToggleState()); };
        soloButton.setToggleState(track->isSolo(false), juce::dontSendNotification);
        addAndMakeVisible(soloButton);
    }

    addAndMakeVisible(levelBar);

    addMouseListener(this, true);                              // hover + click from every child

    ptl::ChannelVisibility::get().addChangeListener(this);     // ticks changed in Devices > audio settings

    populateSelectors();
    attachLevelMeter();
}

TrackRow::~TrackRow()
{
    ptl::ChannelVisibility::get().removeChangeListener(this);

    if (deviceManager != nullptr)
    {
        deviceManager->removeChangeListener(this);
        deviceManager->getAudioDeviceManager().removeChangeListener(this);
    }

    detachLevelMeter();
}

void TrackRow::setSelected(bool shouldBeSelected)
{
    if (selected != shouldBeSelected)
    {
        selected = shouldBeSelected;
        repaint();
    }
}

void TrackRow::setDeviceManager(te::DeviceManager* dm)
{
    if (deviceManager != dm)
    {
        if (deviceManager != nullptr)
        {
            deviceManager->removeChangeListener(this);
            deviceManager->getAudioDeviceManager().removeChangeListener(this);
        }

        deviceManager = dm;

        if (deviceManager != nullptr)
        {
            deviceManager->addChangeListener(this);                           // wave device list rebuilt
            deviceManager->getAudioDeviceManager().addChangeListener(this);   // channels enabled / disabled in Devices
        }
    }

    restoreSavedRouting();
    populateSelectors();
}

// Fired (async, message thread) by te::DeviceManager after it rebuilt its list of
// wave devices - i.e. after the ASIO driver / channels changed.
void TrackRow::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    const bool fromTracktion = (deviceManager != nullptr && source == deviceManager);

    if (fromTracktion && pendingFirstChannel >= 0)
    {
        const int first = pendingFirstChannel, num = pendingNumChannels;
        pendingFirstChannel = -1;
        routeInputChannels(first, num);
    }

    restoreSavedRouting();                    // device opened / list rebuilt: bring back what the .pt says
    populateSelectors();
}

void TrackRow::mouseDown(const juce::MouseEvent& e)
{
    // Select on the card and on the name; combos / buttons keep their own click.
    if (e.eventComponent != this && e.eventComponent != &nameLabel)
        return;

    if (onSelected)
        onSelected(track);
}

void TrackRow::updateHover()
{
    const bool over = isMouseOver(true);
    if (over != hover)
    {
        hover = over;
        repaint();
    }
}

//==============================================================================
void TrackRow::paint(juce::Graphics& g)
{
    auto card = getLocalBounds().toFloat().reduced(0.0f, (float) kCardMargin);

    const auto fill = selected ? (hover ? theme::selectionHover : theme::selection)
                               : (hover ? theme::rowHover       : theme::rowIdle);
    g.setColour(fill);
    g.fillRoundedRectangle(card, theme::radius);

    // Border: subtle by default (hover brightens it a little); red when a heavy plugin
    // is on this track while PDC is on (the plugin stays enabled).
    if (heavy)
    {
        g.setColour(theme::heavyPluginBorder);
        g.drawRoundedRectangle(card.reduced(0.75f), theme::radius, 1.5f);
    }
    else
    {
        g.setColour(selected ? theme::outlineStrong : (hover ? theme::outline : theme::hairline));
        g.drawRoundedRectangle(card.reduced(0.5f), theme::radius, 1.0f);
    }

    if (selected)
    {
        g.setColour(theme::accent);
        g.fillRoundedRectangle(card.getX(), card.getY(), 3.0f, card.getHeight(), 1.5f);
    }

    // Track-type icon: 24x27 box, glyph drawn at 16px
    const auto box = juce::Rectangle<float>((float) kPadL - 1.0f, (float) kPadTop, (float) kIconW, (float) kCtrlH);
    icons::draw(g, iconFor(kind), box.withSizeKeepingCentre(16.0f, 16.0f), colourFor(kind));
}

void TrackRow::resized()
{
    const bool isMaster = (kind == TrackManager::TrackType::Master);
    const bool isFx     = (kind == TrackManager::TrackType::FXChannel);

    const int btnY = kPadTop + (kCtrlH - kBtn) / 2;
    const int comboH = 26;
    const int comboY = kPadTop + (kCtrlH - comboH) / 2;

    // Line 1, laid out right -> left. Button slots are fixed so rows line up (Master has no solo).
    int x = getWidth() - 6;

    auto putButton = [&] (juce::Component& c, bool visible)
    {
        x -= kBtn;
        if (visible)
            c.setBounds(x, btnY, kBtn, kBtn);
        x -= kBtnGap;
    };

    putButton(soloButton,   ! isMaster);
    putButton(muteButton,   true);
    putButton(deleteButton, true);
    x -= 4;                                                   // 6px total gap before the combos

    const bool hasInput = ! isMaster && ! isFx;
    const int lineLeft  = kPadL + kIconW + 4;
    const int avail     = x - lineLeft - 8;
    const int comboW    = hasInput ? juce::jlimit(64, kComboW, (avail - 64 - 8 - kChevronW) / 2)
                                   : juce::jlimit(64, kComboW, avail);

    x -= comboW;
    outputSelector.setBounds(x, comboY, comboW, comboH);

    if (hasInput)
    {
        x -= kChevronW;
        arrowLabel.setBounds(x, comboY, kChevronW, comboH);
        x -= comboW;
        inputSelector.setBounds(x, comboY, comboW, comboH);
    }

    nameLabel.setBounds(lineLeft, kPadTop, juce::jmax(20, x - 8 - lineLeft), kCtrlH);

    // Line 2: slim meter under the name
    levelBar.setBounds(kPadL, kPadTop + kCtrlH + kMeterGap, 80, kMeterH);
}

//==============================================================================
// Level meter ------------------------------------------------------------------
juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> TrackRow::findOrCreateLevelMeter()
{
    if (track == nullptr)
        return {};

    if (auto* audioTrack = dynamic_cast<te::AudioTrack*>(track))
        return audioTrack->getLevelMeterPlugin();

    // Master bus: NOT an AudioTrack, its plugins live in the edit's master plugin list.
    auto& edit = track->edit;
    auto& masterPlugins = edit.getMasterPluginList();

    if (auto* existing = masterPlugins.findFirstPluginOfType<te::LevelMeterPlugin>())
        return existing;

    if (auto plugin = edit.getPluginCache().createNewPlugin(te::LevelMeterPlugin::xmlTypeName, {}))
    {
        masterPlugins.insertPlugin(plugin, -1, nullptr);      // end of chain = post master fader
        return dynamic_cast<te::LevelMeterPlugin*>(plugin.get());
    }

    return {};
}

void TrackRow::attachLevelMeter()
{
    detachLevelMeter();
    meterPlugin = findOrCreateLevelMeter();

    if (meterPlugin != nullptr)
        meterPlugin->measurer.addClient(levelClient);
}

void TrackRow::detachLevelMeter()
{
    if (meterPlugin != nullptr)
    {
        meterPlugin->measurer.removeClient(levelClient);
        meterPlugin = nullptr;
    }
}

void TrackRow::refreshHeavyState()
{
    bool nowHeavy = false;

    if (track != nullptr && AudioEngine::isPDCEnabled())
    {
        for (auto* plugin : trackManager.getPluginList(track).getPlugins())
        {
            if (plugin != nullptr && AudioEngine::isHeavyPlugin(*plugin))
            {
                nowHeavy = true;
                break;
            }
        }
    }

    if (nowHeavy != heavy)
    {
        heavy = nowHeavy;
        repaint();
    }
}

void TrackRow::updateMeter()
{
    // Follow mute / solo changed from outside this row (control bar BEAT / MICRO buttons, other windows):
    // the buttons used to be set once in the constructor, so the red "muted" state never showed up.
    if (track != nullptr && kind != TrackManager::TrackType::Master)
    {
        const bool muted = track->isMuted(false);
        if (muteButton.getToggleState() != muted)
            muteButton.setToggleState(muted, juce::dontSendNotification);

        const bool soloed = track->isSolo(false);
        if (soloButton.getToggleState() != soloed)
            soloButton.setToggleState(soloed, juce::dontSendNotification);
    }

    if (++heavyTick >= 15)                                    // ~2 Hz is plenty for a warning border
    {
        heavyTick = 0;
        refreshHeavyState();
    }

    if (meterPlugin == nullptr)
        attachLevelMeter();

    if (meterPlugin == nullptr)
        return;

    const auto numChans = levelClient.getNumChannelsUsed();

    // Don't log "mono track went stereo" here - the LevelMeter measures the VOLUME/SPAN PLUGIN's output,
    // which is always stereo (see VolumeAndPanPlugin::getBusses()). The actual track INPUT channels
    // are determined by WaveInputDevice and should be monitored separately via routing/logging.
    // This check was kept only for debugging plugin chain stereo conversion issues.
    if (static_cast<int>(numChans) != lastLoggedMeterChans)
    {
        const int before = lastLoggedMeterChans;
        lastLoggedMeterChans = static_cast<int>(numChans);

        ptl::diag::write("[METER] '" + (track != nullptr ? track->getName() : juce::String("?")) + "' now carries "
                        + juce::String(lastLoggedMeterChans) + " channel(s) (was "
                        + (before < 0 ? juce::String("unknown") : juce::String(before)) + ")");

        // Only warn if a MONO IN track genuinely has stereo OUTPUT from its plugin chain
        // (not just because VolumeAndPan reports stereo). Use this to detect routing bugs
        // where a mono track accidentally receives 2 physical input channels.
        if (kind == TrackManager::TrackType::MonoIn && lastLoggedMeterChans >= 2)
        {
            // LOGGING ONLY - for debugging track routing issues
            ptl::diag::write ("[METER] NOTE: 'Mono In' track with stereo meter reading. "
                              "This may indicate routing bug or expected Volume/Span plugin behavior.");
        }
    }

    float left  = -100.0f;                                    // silence / no channel: nothing lit
    float right = -100.0f;

    if (numChans >= 1)
        left = levelClient.getAndClearAudioLevel(0).dB;
    if (numChans >= 2)
        right = levelClient.getAndClearAudioLevel(1).dB;
    else
        right = left;                                         // mono source: show on both lines

    levelBar.setLevels(left, right);

    // Diagnostics: a MONO IN track must show the SAME level on both lines. Every 2 s compare the
    // peaks seen in that window and log only when they differ (and once more when they agree again).
    if (kind == TrackManager::TrackType::MonoIn)
    {
        lrPeakL = juce::jmax(lrPeakL, left);
        lrPeakR = juce::jmax(lrPeakR, right);

        const auto nowMs = juce::Time::getMillisecondCounter();

        if (nowMs - lrLastCheckMs >= 2000)
        {
            lrLastCheckMs = nowMs;

            if (lrPeakL > -60.0f || lrPeakR > -60.0f)               // only while there is signal
            {
                const float diff = std::abs(lrPeakL - lrPeakR);
                const bool mismatch = diff > 1.0f;

                if (mismatch || lrWasMismatch)
                    ptl::diag::write("[LR] '" + (track != nullptr ? track->getName() : juce::String("?"))
                                     + "' peak L=" + juce::String(lrPeakL, 1) + " dB R=" + juce::String(lrPeakR, 1)
                                     + " dB diff=" + juce::String(diff, 1) + " dB"
                                     + (mismatch ? "  <-- L/R MISMATCH" : "  (L/R equal again)"));

                lrWasMismatch = mismatch;
            }

            lrPeakL = lrPeakR = -100.0f;
        }
    }

    if (onLevels)
        onLevels(left, right);
}


//==============================================================================
// Combos -----------------------------------------------------------------------
void TrackRow::populateSelectors()
{
    if (kind == TrackManager::TrackType::Master)
    {
        populateOutputSelector();
        return;
    }

    // Input tracks and FX channels: output is always the Master bus.
    outputSelector.clear(juce::dontSendNotification);
    outputSelector.addItem("MASTER", 1);
    outputSelector.setSelectedId(1, juce::dontSendNotification);
    outputSelector.setEnabled(true);                          // single fixed destination

    if (kind != TrackManager::TrackType::FXChannel)
        populateInputSelector();
}

void TrackRow::populateInputSelector()
{
    inputSelector.clear(juce::dontSendNotification);

    auto* audioTrack = dynamic_cast<te::AudioTrack*>(track);
    if (audioTrack == nullptr)
        return;

    inputSelector.addItem("No Input", noInputItemId);

    if (deviceManager == nullptr)
    {
        inputSelector.setSelectedId(noInputItemId, juce::dontSendNotification);
        updateInputWarning();
        return;
    }

    auto& edit = audioTrack->edit;
    int selectedId = noInputItemId;

    // Physical channel names straight from the active ASIO driver ("In 1", "Link In 3" ...).
    // Only channels that are ENABLED in Devices > audio settings are offered.
    juce::StringArray channelNames;
    juce::BigInteger activeChannels;
    if (auto* audioDevice = deviceManager->getAudioDeviceManager().getCurrentAudioDevice())
    {
        channelNames = audioDevice->getInputChannelNames();

        // The user's ticks (not the dense set the driver streams). No ticks stored yet: fall back.
        auto& vis = ptl::ChannelVisibility::get();
        activeChannels = vis.hasEntry(audioDevice->getName()) ? vis.getInputs(audioDevice->getName())
                                                              : audioDevice->getActiveInputChannels();
    }

    const bool stereo = isStereoTrack();
    const int step = stereo ? 2 : 1;

    auto labelFor = [&] (int ch)
    {
        juce::String label = channelNames[ch];
        if (stereo)
            label << " + " << channelNames[ch + 1];
        return label;
    };

    // Mono   -> one item per channel       : "In 1", "In 2" ...
    // Stereo -> one item per aligned PAIR  : "In 1 + In 2", "Link In 3 + Link In 4" ...
    int numOffered = 0;
    for (int ch = 0; ch + step <= channelNames.size(); ch += step)
    {
        bool allEnabled = true;
        for (int k = 0; k < step; ++k)
            allEnabled = allEnabled && activeChannels[ch + k];

        if (! allEnabled)
            continue;

        inputSelector.addItem(labelFor(ch), firstDeviceItemId + ch);
        ++numOffered;
    }

    if (numOffered == 0)
    {
        inputSelector.addItem("(no input enabled)", placeholderItemId);
        inputSelector.setItemEnabled(placeholderItemId, false);
    }

    // Which channel is this track currently routed from?
    for (auto* dev : deviceManager->getWaveInputDevices())
    {
        if (dev == nullptr)
            continue;

        if (auto* inst = edit.getCurrentInstanceForInputDevice(dev))
        {
            if (! inst->getTargets().contains(audioTrack->itemID))
                continue;

            int firstCh = std::numeric_limits<int>::max();
            for (const auto& ci : dev->getChannels())
                firstCh = juce::jmin(firstCh, static_cast<int>(ci.indexInDevice));

            if (firstCh != std::numeric_limits<int>::max())
                selectedId = firstDeviceItemId + firstCh;
        }
    }

    // Routed to a channel that has since been disabled in Devices: keep showing it (greyed out)
    // instead of silently pretending the track has no input.
    if (selectedId != noInputItemId && inputSelector.indexOfItemId(selectedId) < 0)
    {
        const int ch = selectedId - firstDeviceItemId;
        if (juce::isPositiveAndBelow(ch + step - 1, channelNames.size()))
        {
            inputSelector.addItem(labelFor(ch) + " (off)", selectedId);
            inputSelector.setItemEnabled(selectedId, false);
        }
        else
            selectedId = noInputItemId;
    }

    inputSelector.setSelectedId(selectedId, juce::dontSendNotification);
    updateInputWarning();
}

// "No Input" is a WARNING state, not a neutral choice: a track without input makes no sound on stage.
// The combo turns amber (text, outline, tinted body, bold) and gets a tooltip. PtLiveLookAndFeel reads the "warn" property.
void TrackRow::updateInputWarning()
{
    const bool warn = inputSelector.isVisible() && inputSelector.getSelectedId() == noInputItemId;

    inputSelector.getProperties().set("warn", warn);
    inputSelector.setColour(juce::ComboBox::textColourId,       warn ? theme::warning : theme::text);
    inputSelector.setColour(juce::ComboBox::outlineColourId,    warn ? theme::warning : theme::outline);
    inputSelector.setColour(juce::ComboBox::backgroundColourId, warn ? theme::raised.overlaidWith(theme::warning.withAlpha(0.16f)) : theme::raised);
    inputSelector.setColour(juce::ComboBox::arrowColourId,      warn ? theme::warning : theme::textDim);
    inputSelector.setTooltip(warn ? juce::String::fromUTF8("Ch\xC6\xB0" "a ch\xE1\xBB\x8Dn input: track n\xC3\xA0y s\xE1\xBA\xBD kh\xC3\xB4ng c\xC3\xB3 ti\xE1\xBA\xBFng")
                                  : juce::String());
    inputSelector.resized();                       // label indent / font follow the state
    inputSelector.repaint();
}

void TrackRow::populateOutputSelector()
{
    outputSelector.clear(juce::dontSendNotification);
    outputDeviceIDs.clear();
    outputSelector.setEnabled(true);

    auto* audioDevice = deviceManager != nullptr ? deviceManager->getAudioDeviceManager().getCurrentAudioDevice()
                                                 : nullptr;
    if (audioDevice == nullptr)
    {
        outputSelector.addItem("(no output available)", placeholderItemId);
        outputSelector.setItemEnabled(placeholderItemId, false);
        outputSelector.setSelectedId(placeholderItemId, juce::dontSendNotification);
        return;
    }

    const auto defaultID = deviceManager->getDefaultWaveOutDeviceID();
    auto& vis = ptl::ChannelVisibility::get();
    const auto activeOut = vis.hasEntry(audioDevice->getName()) ? vis.getOutputs(audioDevice->getName())
                                                                : audioDevice->getActiveOutputChannels();
    int selectedId = 0;

    // Default grouping is stereo pairs: "Out 1 + Out 2", "Out 3 + Out 4" ...
    // Only outputs whose channels are ALL enabled in Devices > audio settings are offered.
    for (auto* dev : deviceManager->getWaveOutputDevices())
    {
        if (dev == nullptr)
            continue;

        bool allEnabled = dev->getChannels().getNumChannels() > 0;
        for (const auto& ci : dev->getChannels())
            allEnabled = allEnabled && activeOut[static_cast<int>(ci.indexInDevice)];

        if (! allEnabled)
            continue;

        const int itemId = firstDeviceItemId + outputDeviceIDs.size();
        outputDeviceIDs.add(dev->getDeviceID());
        outputSelector.addItem(dev->getAliasOrName(), itemId);

        if (dev->getDeviceID() == defaultID && dev->isEnabled())
            selectedId = itemId;
    }

    if (outputDeviceIDs.isEmpty())
    {
        outputSelector.addItem("(no output enabled)", placeholderItemId);
        outputSelector.setItemEnabled(placeholderItemId, false);
        selectedId = placeholderItemId;
    }

    if (selectedId != 0)
        outputSelector.setSelectedId(selectedId, juce::dontSendNotification);
    else
        outputSelector.setText("Select Output", juce::dontSendNotification);
}

//==============================================================================
// Routing ----------------------------------------------------------------------
// Make sure the JUCE audio device actually delivers the channels of this wave input.
// NOTE: Tracktion indexes the audio buffers by the channel's physical index on the card,
// but JUCE hands the callback a buffer holding ONLY the active channels, packed from 0.
// So the active set must be DENSE (0..N).
static void ensureJuceInputChannelsActive(te::DeviceManager& dm, const te::WaveInputDevice& dev)
{
    auto& adm = dm.getAudioDeviceManager();
    auto* audioDevice = adm.getCurrentAudioDevice();
    if (audioDevice == nullptr)
        return;

    // P0: during the app's once-only startup ASIO open, all required channels were already
    // unioned into applyASIOConfig and opened in one shot. Do NOT restart the device here - that
    // is what caused the repeated ASIO reloads at launch. The channels this track needs are
    // already active (or will be activated by the single startup open).
    if (ptl::isAudioEngineInStartupPhase())
        return;

    // Only THIS device's own channels are activated (sparse, like Cubase); SparseChannelAdapter
    // maps JUCE's packed buffers back to physical channel numbers for Tracktion.
    auto active = audioDevice->getActiveInputChannels();
    bool changed = false;
    bool any = false;

    for (const auto& ci : dev.getChannels())
    {
        const int idx = static_cast<int>(ci.indexInDevice);
        if (idx < 0)
            continue;

        any = true;
        if (! active[idx])
        {
            active.setBit(idx);
            changed = true;
        }
    }

    if (! any)
        return;

    if (! changed)
        return;

    auto setup = adm.getAudioDeviceSetup();
    setup.inputChannels = active;
    setup.useDefaultInputChannels = false;

    AudioEngine::SetupScope setupScope;      // app-made restart, not a driver reset request
    auto err = adm.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
        DBG("TrackRow: could not activate input channels: " << err);

    // Flush Tracktion's async wave-device rebuild so the new channel is live immediately.
    dm.dispatchPendingUpdates();
}

static void ensureJuceOutputChannelsActive(te::DeviceManager& dm, const te::WaveOutputDevice& dev)
{
    auto& adm = dm.getAudioDeviceManager();
    auto* audioDevice = adm.getCurrentAudioDevice();
    if (audioDevice == nullptr)
        return;

    // P0: same as the input side - no device restarts while the startup open is running.
    if (ptl::isAudioEngineInStartupPhase())
        return;

    // Only THIS device's own channels are activated (sparse, like Cubase); SparseChannelAdapter
    // maps JUCE's packed buffers back to physical channel numbers for Tracktion.
    auto active = audioDevice->getActiveOutputChannels();
    bool changed = false;
    bool any = false;

    for (const auto& ci : dev.getChannels())
    {
        const int idx = static_cast<int>(ci.indexInDevice);
        if (idx < 0)
            continue;

        any = true;
        if (! active[idx])
        {
            active.setBit(idx);
            changed = true;
        }
    }

    if (! any)
        return;

    if (! changed)
        return;

    auto setup = adm.getAudioDeviceSetup();
    setup.outputChannels = active;
    setup.useDefaultOutputChannels = false;

    AudioEngine::SetupScope setupScope;      // app-made restart, not a driver reset request
    auto err = adm.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
        DBG("TrackRow: could not activate output channels: " << err);

    // Flush Tracktion's async wave-device rebuild so the new channel is live immediately.
    dm.dispatchPendingUpdates();
}

void TrackRow::clearTrackRouting(te::AudioTrack& audioTrack)
{
    auto& edit = audioTrack.edit;

    for (auto* inst : edit.getAllInputDevices())
    {
        if (inst == nullptr)
            continue;

        // Only physical wave inputs - ignore MIDI and the track's own internal device.
        if (inst->getInputDevice().getDeviceType() != te::InputDevice::waveDevice)
            continue;

        if (inst->getTargets().contains(audioTrack.itemID))
        {
            inst->setRecordingEnabled(audioTrack.itemID, false);
            [[maybe_unused]] auto res = inst->removeTarget(audioTrack.itemID, &edit.getUndoManager());
        }
    }
}

bool TrackRow::routeTrackTo(te::AudioTrack& audioTrack, te::WaveInputDevice& dev)
{
    auto& edit = audioTrack.edit;

    auto* inst = edit.getCurrentInstanceForInputDevice(&dev);
    if (inst == nullptr)
        return false;

    // moveToTrack = false: the same physical input may feed several tracks.
    auto res = inst->setTarget(audioTrack.itemID, false, &edit.getUndoManager());
    if (! res.has_value())
    {
        DBG("TrackRow: setTarget failed: " << res.error());
        return false;
    }

    // Arm the track so the input is monitored live.
    inst->setRecordingEnabled(audioTrack.itemID, true);
    return true;
}

void TrackRow::handleOutputSelectionChanged()
{
    if (deviceManager == nullptr)
        return;

    const int index = outputSelector.getSelectedId() - firstDeviceItemId;
    if (! juce::isPositiveAndBelow(index, outputDeviceIDs.size()))
        return;

    const juce::String deviceID = outputDeviceIDs[index];

    auto find = [this, &deviceID]() -> te::WaveOutputDevice*
    {
        for (auto* d : deviceManager->getWaveOutputDevices())
            if (d != nullptr && d->getDeviceID() == deviceID)
                return d;
        return nullptr;
    };

    auto* dev = find();
    if (dev == nullptr)
        return;

    if (! dev->isEnabled())
        dev->setEnabled(true);

    ensureJuceOutputChannelsActive(*deviceManager, *dev);

    // Device list may have been rebuilt - look it up again, then make it the default output.
    if ((dev = find()) != nullptr && dev->isEnabled())
    {
        deviceManager->setDefaultWaveOutDevice(deviceID);
        track->state.setProperty(kOutDevId, deviceID, nullptr);       // saved with the session
        restoreTries = 0;
    }

    // NOTE: no restartPlayback() here. Tracktion's transport picks up the new default
    // output automatically; forcing a graph rebuild on every output combo change caused
    // audio dropouts / glitches. The graph is rebuilt only when needed (e.g. driver change).
}

// Routes physical channels [firstChannel, firstChannel + numChannels) to this track.
// CHUẨN DAW (Cubase/Logic Pro):
//   1. Tìm WaveInputDevice chứa firstChannel (Tracktion group theo physical channel)
//   2. Enable device, đảm bảo ASIO driver stream channels
//   3. Route track đến device
//   4. Tracktion ENGINE tự set ChannelConfiguration dựa trên track type (mono/stereo) khi build graph
//      (createLiveInputNodeForDevice -> waveDevice->setChannelConfiguration(sourceTrack->getChannelConfiguration()))
//
// LƯU Ý QUAN TRỌNG: Chúng ta KHÔNG ép device channels bằng setDeviceNumChannels() nữa.
// Tracktion sẽ tự set channel configuration dựa trên track type khi build audio graph.
// Việc ép channels gây conflict với Tracktion's internal routing -> deadlock.
bool TrackRow::routeInputChannels(int firstChannel, int numChannels)
{
    auto* audioTrack = dynamic_cast<te::AudioTrack*>(track);
    if (audioTrack == nullptr || deviceManager == nullptr)
        return false;

    for (int attempt = 0; attempt < 6; ++attempt)
    {
        te::WaveInputDevice* container = nullptr;
        int  containerFirst = 0;
        bool exact = false;

        // getWaveInputDevices() flushes any pending async rebuild, so pointers are fresh.
        for (auto* dev : deviceManager->getWaveInputDevices())
        {
            if (dev == nullptr)
                continue;

            int lo = std::numeric_limits<int>::max(), hi = -1;
            for (const auto& ci : dev->getChannels())
            {
                lo = juce::jmin(lo, static_cast<int>(ci.indexInDevice));
                hi = juce::jmax(hi, static_cast<int>(ci.indexInDevice));
            }

            if (firstChannel < lo || firstChannel > hi)
                continue;

            container      = dev;
            containerFirst = lo;
            exact = (lo == firstChannel && dev->getChannels().getNumChannels() == numChannels);
            break;
        }

        if (container == nullptr)
            break;

        // CHUẨN DAW: device container có thể là 1 cặp stereo (vd ASIO Link Pro nhóm IN 1-2 = 1 device).
        // Mono In track (numChannels=1) phải tách đúng 1 kênh khỏi cặp đó; Stereo In (numChannels=2) giữ cặp.
        // Nếu container không khớp (firstChannel nằm trong cặp nhưng không phải kênh đầu, hoặc số kênh sai):
        // regroup lại device rồi lặp - danh sách device sẽ được rebuild.
        if (! exact)
        {
            const int n = (containerFirst == firstChannel) ? numChannels
                                                          : firstChannel - containerFirst;
            deviceManager->setDeviceNumChannels(*container, static_cast<uint32_t>(n));
            continue;   // device list được rebuild -> lặp lại với container mới
        }

        // Enable the device if not already
        if (! container->isEnabled())
        {
            container->setEnabled(true);
            continue;
        }

        // Make sure the ASIO driver streams this channel (dense active set)
        ensureJuceInputChannelsActive(*deviceManager, *container);
        audioTrack->edit.getTransport().ensureContextAllocated();

        if (routeTrackTo(*audioTrack, *container))
            return true;

        break;
    }

    // Not ready yet: retry when DeviceManager announces the rebuilt list.
    pendingFirstChannel = firstChannel;
    pendingNumChannels  = numChannels;
    return false;
}

void TrackRow::handleInputSelectionChanged()
{
    updateInputWarning();

    auto* audioTrack = dynamic_cast<te::AudioTrack*>(track);

    if (audioTrack == nullptr || deviceManager == nullptr)
        return;

    pendingFirstChannel = -1;
    audioTrack->edit.getTransport().ensureContextAllocated();

    // Always drop the previous routing first.
    clearTrackRouting(*audioTrack);
    restoreTries = 0;

    const int id = inputSelector.getSelectedId();
    if (id < firstDeviceItemId)
    {
        track->state.removeProperty(kInFirstId, nullptr);
        track->state.setProperty(kInNoneId, true, nullptr);       // explicit "No Input": never restored
        if (onInputChangedByUser)
            onInputChangedByUser(track);
        return;
    }

    // CHUẨN DAW: Mono/Stereo track loại quyết định số channel, không phải user chọn
    // Giống Cubase/Logic Pro: Mono In track LUÔN dùng 1 input channel, Stereo In LUÔN dùng 2
    const int numCh = kind == TrackManager::TrackType::MonoIn ? 1 : 2;

    track->state.setProperty(kInFirstId, id - firstDeviceItemId, nullptr);   // saved with the session
    // DON'T SAVE kInNumId - always derive from track type instead of saved value
    track->state.setProperty(kInNoneId, false, nullptr);

    routeInputChannels(id - firstDeviceItemId, numCh);

    if (onInputChangedByUser)
        onInputChangedByUser(track);          // MainWindow reloads the driver once (coalesced)
}

//==============================================================================
bool TrackRow::isRoutedFromAnyInput() const
{
    auto* at = dynamic_cast<te::AudioTrack*>(track);
    if (at == nullptr || deviceManager == nullptr)
        return false;

    for (auto* dev : deviceManager->getWaveInputDevices())
        if (dev != nullptr)
            if (auto* inst = at->edit.getCurrentInstanceForInputDevice(dev))
                if (inst->getTargets().contains(at->itemID))
                    return true;

    return false;
}

void TrackRow::reapplyInputRouting()
{
    restoreTries    = 0;
    lastRestoreOkMs = 0;

    // Deferred: we are still inside the plugin-insert call stack, let the new graph settle first.
    juce::Component::SafePointer<TrackRow> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->restoreSavedRouting();
    });
}

// Called when the row gets its device manager and every time the audio device / wave device list changes
// (that is also when the ASIO driver finally opens after the session was loaded).
void TrackRow::restoreSavedRouting()
{
    if (deviceManager == nullptr || pendingFirstChannel >= 0 || track == nullptr)
        return;

    auto* audioDevice = deviceManager->getAudioDeviceManager().getCurrentAudioDevice();
    if (audioDevice == nullptr)
        return;                                                   // driver not open yet: wait for the next change

    //== Master: the output the user picked ==================================================
    if (kind == TrackManager::TrackType::Master)
    {
        const auto savedId = track->state.getProperty(kOutDevId).toString();
        if (savedId.isEmpty() || savedId == deviceManager->getDefaultWaveOutDeviceID())
        {
            restoreTries = 0;
            return;
        }

        if (restoreTries >= 6)
            return;
        ++restoreTries;

        for (auto* dev : deviceManager->getWaveOutputDevices())
            if (dev != nullptr && dev->getDeviceID() == savedId)
            {
                if (! dev->isEnabled())
                    dev->setEnabled(true);

                ensureJuceOutputChannelsActive(*deviceManager, *dev);

                for (auto* d : deviceManager->getWaveOutputDevices())      // the list may have been rebuilt
                    if (d != nullptr && d->getDeviceID() == savedId && d->isEnabled())
                    {
                        deviceManager->setDefaultWaveOutDevice(savedId);
                        break;
                    }
                break;
            }
        return;
    }

    //== Input tracks: the input channel(s) the user picked ==================================
    auto* audioTrack = dynamic_cast<te::AudioTrack*>(track);
    if (audioTrack == nullptr || kind == TrackManager::TrackType::FXChannel)
        return;

    if (! track->state.hasProperty(kInFirstId) || static_cast<bool>(track->state.getProperty(kInNoneId, false)))
        return;

    if (isRoutedFromAnyInput())
    {
        restoreTries = 0;
        return;
    }

    if (restoreTries >= 6)
        return;

    const int first = static_cast<int>(track->state.getProperty(kInFirstId));
    // CHUẨN DAW: Mono/Stereo track loại quyết định số channel - không bao giờ đọc từ session file!
    // Giống Cubase/Logic Pro: Mono In track LUÔN dùng 1 channel, Stereo In LUÔN dùng 2 channels
    const int num      = kind == TrackManager::TrackType::MonoIn ? 1 : 2;

    if (first < 0 || first + num > audioDevice->getInputChannelNames().size())
        return;                                                   // this driver does not have that channel

    // te::DeviceManager and the AudioDeviceManager both broadcast when the ASIO driver opens, so this
    // ran twice in 1 ms: the second route restarted the device while the first one was still being
    // applied (graph rebuild under the running audio callback). One successful restore is enough.
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    if (lastRestoreOkMs != 0 && nowMs - static_cast<double>(lastRestoreOkMs) < 1500.0)
        return;

    ++restoreTries;
    ptl::diag::write("[routing] restoring input of '" + track->getName() + "': channel " + juce::String(first + 1)
                     + " (" + juce::String(num) + " ch) from the session");
    if (routeInputChannels(first, num))
        lastRestoreOkMs = static_cast<juce::int64>(nowMs);
}
