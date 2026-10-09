/*
    pt.Live - Live Singing DAW
    TrackStrip implementation

    Single track control panel with volume, mute, VU meter.
*/

#include "include/UI/TrackStrip.h"
#include "include/UI/VUMeter.h"
#include "include/Theme.h"

#include <limits>

using namespace ptl;

TrackStrip::TrackStrip(TrackManager& mgr, te::Track* trk)
    : trackManager(mgr), track(trk)
{
    if (track != nullptr)
    {
        nameLabel.setText(track->getName(), juce::dontSendNotification);
    }
    else
    {
        nameLabel.setText("Unnamed Track", juce::dontSendNotification);
    }

    setupLabels();
    setupControls();

    addAndMakeVisible(nameLabel);
    addAndMakeVisible(inputSelector);
    addAndMakeVisible(volumeSlider);
    addAndMakeVisible(volumeValueLabel);
    addAndMakeVisible(muteButton);
    addAndMakeVisible(soloButton);
    addAndMakeVisible(*vuMeter);

    // Register a client on the level meter plugin (audio track OR master bus).
    attachLevelMeter();

    // Click selects this track for the inspector panel
    setInterceptsMouseClicks(true, true);

    startTimerHz(VUMeter::meterHz);  // 20Hz meter + status refresh
}

TrackStrip::~TrackStrip()
{
    if (deviceManager != nullptr)
        deviceManager->removeChangeListener(this);

    detachLevelMeter();
}

juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> TrackStrip::findOrCreateLevelMeter()
{
    if (track == nullptr)
        return {};

    if (auto* audioTrack = dynamic_cast<te::AudioTrack*>(track))
        return audioTrack->getLevelMeterPlugin();

    // Master bus: it is NOT an AudioTrack, its plugins live in the edit's master plugin list.
    auto& edit = track->edit;
    auto& masterPlugins = edit.getMasterPluginList();

    if (auto* existing = masterPlugins.findFirstPluginOfType<te::LevelMeterPlugin>())
        return existing;

    // None yet: append one at the END of the master chain (= post master fader).
    if (auto plugin = edit.getPluginCache().createNewPlugin(te::LevelMeterPlugin::xmlTypeName, {}))
    {
        masterPlugins.insertPlugin(plugin, -1, nullptr);
        return dynamic_cast<te::LevelMeterPlugin*>(plugin.get());
    }

    return {};
}

void TrackStrip::attachLevelMeter()
{
    detachLevelMeter();
    meterPlugin = findOrCreateLevelMeter();

    if (meterPlugin != nullptr)
        meterPlugin->measurer.addClient(levelClient);
}

void TrackStrip::detachLevelMeter()
{
    if (meterPlugin != nullptr)
    {
        meterPlugin->measurer.removeClient(levelClient);
        meterPlugin = nullptr;
    }
}

void TrackStrip::setDeviceManager(te::DeviceManager* dm)
{
    if (deviceManager != dm)
    {
        if (deviceManager != nullptr)
            deviceManager->removeChangeListener(this);

        deviceManager = dm;

        if (deviceManager != nullptr)
            deviceManager->addChangeListener(this);
    }

    populateInputSelector();
}

// Fired (async, message thread) by te::DeviceManager after it has rebuilt its
// list of WaveInputDevices - i.e. after the ASIO driver / channels changed.
void TrackStrip::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // A routing request that could not finish earlier: retry now the device list is rebuilt.
    if (pendingFirstChannel >= 0)
    {
        const int first = pendingFirstChannel, num = pendingNumChannels;
        pendingFirstChannel = -1;
        routeInputChannels(first, num);
    }

    populateInputSelector();
}

bool TrackStrip::isStereoTrack() const
{
    // TrackManager::createTrack() sets maxNumOfInputs = 2 for "Stereo In", 1 for "Mono In".
    if (auto* audioTrack = dynamic_cast<te::AudioTrack*>(track))
        return audioTrack->getMaxNumOfInputs() >= 2;

    return false;
}

void TrackStrip::populateInputSelector()
{
    inputSelector.clear(juce::dontSendNotification);
    inputDeviceIDs.clear();

    auto* audioTrack = dynamic_cast<te::AudioTrack*>(track);

    // Master track has no input - its combo selects the hardware OUTPUT instead.
    if (audioTrack == nullptr)
    {
        populateOutputSelector();
        return;
    }

    inputSelector.addItem("No Input", noInputItemId);

    inputSelector.setEnabled(true);

    if (deviceManager == nullptr)
    {
        inputSelector.setSelectedId(noInputItemId, juce::dontSendNotification);
        return;
    }

    auto& edit = audioTrack->edit;
    int selectedId = noInputItemId;

    // Physical channel names straight from the active ASIO driver
    // (e.g. "In 1", "In 2", "Link In 3", "Link In 4" for ASIO Link Pro).
    juce::StringArray channelNames;
    if (auto* audioDevice = deviceManager->getAudioDeviceManager().getCurrentAudioDevice())
        channelNames = audioDevice->getInputChannelNames();

    const bool stereo = isStereoTrack();
    const int step = stereo ? 2 : 1;

    // Mono track   -> one item per channel          : "In 1", "In 2", "Link In 3" ...
    // Stereo track -> one item per aligned PAIR      : "In 1 - In 2", "Link In 3 - Link In 4" ...
    for (int ch = 0; ch + step <= channelNames.size(); ch += step)
    {
        juce::String label = channelNames[ch];
        if (stereo)
            label << "  -  " << channelNames[ch + 1];

        inputSelector.addItem(label, firstDeviceItemId + ch);
    }

    if (channelNames.isEmpty())
    {
        inputSelector.addItem("(no input channels - pick an ASIO driver)", placeholderItemId);
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

    inputSelector.setSelectedId(selectedId, juce::dontSendNotification);
}

void TrackStrip::populateOutputSelector()
{
    inputSelector.clear(juce::dontSendNotification);
    inputDeviceIDs.clear();
    inputSelector.setEnabled(true);

    if (deviceManager == nullptr)
    {
        inputSelector.addItem("No Output", placeholderItemId);
        inputSelector.setItemEnabled(placeholderItemId, false);
        inputSelector.setSelectedId(placeholderItemId, juce::dontSendNotification);
        return;
    }

    const auto defaultID = deviceManager->getDefaultWaveOutDeviceID();
    int selectedId = 0;

    // Default grouping is stereo pairs: "Out 1 + Out 2", "Out 3 + Out 4" ...
    for (auto* dev : deviceManager->getWaveOutputDevices())
    {
        if (dev == nullptr)
            continue;

        const int itemId = firstDeviceItemId + inputDeviceIDs.size();
        inputDeviceIDs.add(dev->getDeviceID());
        inputSelector.addItem(dev->getAliasOrName(), itemId);

        if (dev->getDeviceID() == defaultID && dev->isEnabled())
            selectedId = itemId;
    }

    if (inputDeviceIDs.isEmpty())
    {
        inputSelector.addItem("(no outputs - pick an ASIO driver)", placeholderItemId);
        inputSelector.setItemEnabled(placeholderItemId, false);
        selectedId = placeholderItemId;
    }

    if (selectedId != 0)
        inputSelector.setSelectedId(selectedId, juce::dontSendNotification);
    else
        inputSelector.setText("Select Output", juce::dontSendNotification);
}

void TrackStrip::setupLabels()
{
    nameLabel.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
    nameLabel.setColour(juce::Label::textColourId, theme::textPrimary);

    inputSelector.addItem("No Input", 1);
    inputSelector.setSelectedItemIndex(0);
    inputSelector.onChange = [this] { handleInputSelectionChanged(); };

    volumeSlider.setRange(-60.0, 6.0, 0.1);
    volumeSlider.setValue(-6.0);  // nominal default
    volumeSlider.setSliderStyle(juce::Slider::LinearVertical);
    volumeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
    volumeSlider.setColour(juce::Slider::thumbColourId, theme::accentBlue);
    volumeSlider.setColour(juce::Slider::trackColourId, theme::textDimmed);
    volumeSlider.setColour(juce::Slider::backgroundColourId, theme::bgTertiary);

    volumeValueLabel.setColour(juce::Label::textColourId, theme::textSecondary);
    volumeValueLabel.setText("-6.0 dB", juce::dontSendNotification);

    muteButton.setButtonText("MUTE");
    muteButton.setClickingTogglesState(true);
    muteButton.setColour(juce::TextButton::buttonOnColourId, theme::accentRed);
    muteButton.setColour(juce::TextButton::buttonColourId, theme::bgTertiary);
    muteButton.setColour(juce::TextButton::textColourOnId, theme::textPrimary);

    soloButton.setButtonText("SOLO");
    soloButton.setClickingTogglesState(true);
    soloButton.setColour(juce::TextButton::buttonOnColourId, theme::accentBlue);
    soloButton.setColour(juce::TextButton::buttonColourId, theme::bgTertiary);
    soloButton.setColour(juce::TextButton::textColourOnId, theme::textPrimary);
}

void TrackStrip::setupControls()
{
    vuMeter = std::make_unique<VUMeter>();
    vuMeter->setVisible(true);
    vuMeter->startUpdating();
}

void TrackStrip::resized()
{
    auto r = getLocalBounds().reduced(6);

    // Name bar (top)
    nameLabel.setBounds(r.removeFromTop(22));

    // Input selector
    inputSelector.setBounds(r.removeFromTop(20).reduced(0, 2));

    // Middle: VU meter (left) + volume slider (right)
    auto midArea = r.removeFromTop(r.getHeight() - 50); // leave room for buttons + value
    auto vuArea = midArea.removeFromLeft(64);
    vuMeter->setBounds(vuArea);
    volumeSlider.setBounds(midArea.reduced(4, 0));

    // Volume value label
    volumeValueLabel.setBounds(r.removeFromTop(16));

    // Mute/Solo row
    auto btnRow = r.removeFromTop(28);
    muteButton.setBounds(btnRow.removeFromLeft(btnRow.getWidth() / 2 - 3));
    soloButton.setBounds(btnRow.removeFromLeft(btnRow.getWidth() - 3));
}

void TrackStrip::paint(juce::Graphics& g)
{
    g.fillAll(theme::bgSecondary);

    // Border separator from adjacent tracks
    g.setColour(theme::borderDark);
    g.fillRect(juce::Rectangle<int>(getWidth() - 1, 0, 1, getHeight()));
}

void TrackStrip::timerCallback()
{
    if (! isShowing())          // minimised / hidden: skip meter + label work
        return;

    updateMeterFromTrack();

    // Sync volume slider readout (setText only when the text really changed: no needless repaint)
    const float db = static_cast<float>(volumeSlider.getValue());
    const auto txt = juce::String(db, 1) + " dB";
    if (volumeValueLabel.getText() != txt)
        volumeValueLabel.setText(txt, juce::dontSendNotification);
}

void TrackStrip::updateMeterFromTrack()
{
    if (meterPlugin == nullptr)
        attachLevelMeter();          // retry (e.g. master chain not ready at construction)

    if (meterPlugin == nullptr || vuMeter == nullptr)
        return;

    const auto numChans = levelClient.getNumChannelsUsed();
    float leftLevel  { -100.0f };       // silence / no channel: nothing lit
    float rightLevel { -100.0f };

    if (numChans >= 1)
        leftLevel = levelClient.getAndClearAudioLevel(0).dB;
    if (numChans >= 2)
        rightLevel = levelClient.getAndClearAudioLevel(1).dB;

    vuMeter->setCurrentLevels(leftLevel, rightLevel);
}

// Make sure the JUCE audio device actually delivers the channels of this wave
// input (ASIO only streams channels that are "active" in the device setup).
// NOTE: Tracktion indexes the audio buffers by the channel's physical index on the
// card (WaveDeviceDescription::indexInDevice), but JUCE hands the callback a buffer
// holding ONLY the active channels, packed from 0. So the active set must be DENSE
// (0..N) or e.g. "Out 3/4" would be written to the wrong/non-existent buffer = silence.
static void ensureJuceInputChannelsActive(te::DeviceManager& dm, const te::WaveInputDevice& dev)
{
    auto& adm = dm.getAudioDeviceManager();
    auto* audioDevice = adm.getCurrentAudioDevice();
    if (audioDevice == nullptr)
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

    auto err = adm.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
        DBG("TrackStrip: could not activate input channels: " << err);

    // Flush Tracktion's async wave-device rebuild so the new channel is live immediately.
    dm.dispatchPendingUpdates();
}

void TrackStrip::clearTrackRouting(te::AudioTrack& audioTrack)
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

bool TrackStrip::routeTrackTo(te::AudioTrack& audioTrack, te::WaveInputDevice& dev)
{
    auto& edit = audioTrack.edit;

    auto* inst = edit.getCurrentInstanceForInputDevice(&dev);
    if (inst == nullptr)
        return false;

    // moveToTrack = false: the same physical input may feed several tracks.
    auto res = inst->setTarget(audioTrack.itemID, false, &edit.getUndoManager());
    if (! res.has_value())
    {
        DBG("TrackStrip: setTarget failed: " << res.error());
        return false;
    }

    // Arm the track so the input is monitored live (monitor mode = automatic).
    inst->setRecordingEnabled(audioTrack.itemID, true);
    return true;
}

// NOTE: Tracktion indexes the audio buffers by the channel's physical index on the
// card (WaveDeviceDescription::indexInDevice), but JUCE hands the callback a buffer
// holding ONLY the active channels, packed from 0. So the active set must be DENSE
// (0..N) or e.g. "Out 3/4" would be written to the wrong/non-existent buffer = silence.
static void ensureJuceOutputChannelsActive(te::DeviceManager& dm, const te::WaveOutputDevice& dev)
{
    auto& adm = dm.getAudioDeviceManager();
    auto* audioDevice = adm.getCurrentAudioDevice();
    if (audioDevice == nullptr)
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

    auto err = adm.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty())
        DBG("TrackStrip: could not activate output channels: " << err);

    // Flush Tracktion's async wave-device rebuild so the new channel is live immediately.
    dm.dispatchPendingUpdates();
}

void TrackStrip::handleOutputSelectionChanged()
{
    if (deviceManager == nullptr)
        return;

    const int index = inputSelector.getSelectedId() - firstDeviceItemId;
    if (! juce::isPositiveAndBelow(index, inputDeviceIDs.size()))
        return;

    const juce::String deviceID = inputDeviceIDs[index];

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

    // Device list may have been rebuilt - look it up again, then make it the
    // default output: the Master bus (and every track not routed elsewhere) feeds it.
    if ((dev = find()) != nullptr && dev->isEnabled())
        deviceManager->setDefaultWaveOutDevice(deviceID);

    // NOTE: no restartPlayback() here either. Tracktion picks up new defaults automatically;
    // forcing a graph rebuild on every output combo change caused dropouts/glitches.
}

// Routes physical channels [firstChannel, firstChannel + numChannels) to this track.
//
// Tracktion groups physical channels into WaveInputDevices (mono or stereo pair).
// To get exactly the group requested we regroup the one device that contains the
// first channel (split a pair for a mono track, merge two monos for a stereo track),
// enable it, make sure the ASIO driver streams it, then attach it to the track.
bool TrackStrip::routeInputChannels(int firstChannel, int numChannels)
{
    auto* audioTrack = dynamic_cast<te::AudioTrack*>(track);
    if (audioTrack == nullptr || deviceManager == nullptr)
        return false;

    for (int attempt = 0; attempt < 6; ++attempt)
    {
        te::WaveInputDevice* container = nullptr;
        bool exact = false;
        int containerFirst = 0;

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

            container = dev;
            containerFirst = lo;
            exact = (lo == firstChannel && dev->getChannels().getNumChannels() == numChannels);
            break;
        }

        if (container == nullptr)
            break;

        if (! exact)
        {
            // Regroup: shrink/grow the group so it starts at firstChannel with numChannels.
            const int n = (containerFirst == firstChannel) ? numChannels
                                                          : firstChannel - containerFirst;
            deviceManager->setDeviceNumChannels(*container, static_cast<uint32_t>(n));
            continue;
        }

        if (! container->isEnabled())
        {
            container->setEnabled(true);
            continue;
        }

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

void TrackStrip::handleInputSelectionChanged()
{
    auto* audioTrack = dynamic_cast<te::AudioTrack*>(track);

    if (audioTrack == nullptr)          // Master strip: combo = output selector
    {
        handleOutputSelectionChanged();
        return;
    }

    if (deviceManager == nullptr)
        return;

    pendingFirstChannel = -1;
    audioTrack->edit.getTransport().ensureContextAllocated();

    // Always drop the previous routing first.
    clearTrackRouting(*audioTrack);

    const int id = inputSelector.getSelectedId();
    if (id < firstDeviceItemId)
        return;                                   // "No Input"

    routeInputChannels(id - firstDeviceItemId, isStereoTrack() ? 2 : 1);
}

//==============================================================================
// Volume/mute handlers are set up via callbacks in MainWindow/MixerBoard.
// For now, this is a display-only strip. Full control wiring comes in Phase 5.
