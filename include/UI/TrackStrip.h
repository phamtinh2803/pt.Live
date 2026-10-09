/*
    pt.Live - Live Singing DAW
    TrackStrip - individual track control panel

    Contains:
    - Track name display
    - Input selector combo box
    - Volume slider (vertical)
    - Mute/Solo toggle buttons
    - VU meter (L/R columns)
    - Plugin slots placeholder
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/TrackManager.h"

class VUMeter;

class TrackStrip : public juce::Component,
                   private juce::Timer,
                   private juce::ChangeListener
{
public:
    TrackStrip(TrackManager& mgr, te::Track* track);
    ~TrackStrip() override;

    te::Track* getTrack() const { return track; }

    void resized() override;
    void paint(juce::Graphics& g) override;

    // Provide device manager so we can populate input channels.
    // The strip listens to it and re-populates automatically whenever the
    // audio driver / channel list changes (e.g. user picks ASIO Link Pro).
    void setDeviceManager(te::DeviceManager* dm);

    // Selection callback
    std::function<void(te::Track*)> onTrackSelected;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void setupLabels();
    void setupControls();
    void updateMeterFromTrack();
    void attachLevelMeter();                  // audio track AND master bus
    void detachLevelMeter();
    juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> findOrCreateLevelMeter();
    void populateInputSelector();
    void populateOutputSelector();            // Master strip: hardware output (headphones/monitors)
    void handleInputSelectionChanged();
    void handleOutputSelectionChanged();

    // Input routing helpers (physical wave input -> this track)
    bool isStereoTrack() const;                          // stereo track -> channel PAIRS, mono -> single channels
    bool routeInputChannels(int firstChannel, int numChannels);
    void clearTrackRouting(te::AudioTrack&);
    bool routeTrackTo(te::AudioTrack&, te::WaveInputDevice&);

    TrackManager& trackManager;
    te::DeviceManager* deviceManager { nullptr };
    te::Track* track { nullptr };

    // Combo item ids: 1 = "No Input", 100+n = physical input channel n (first channel of the mono channel / stereo pair)
    static constexpr int noInputItemId     = 1;
    static constexpr int placeholderItemId = 2;
    static constexpr int firstDeviceItemId = 100;
    juce::StringArray inputDeviceIDs;          // index n -> WaveInputDevice::getDeviceID() (or WaveOutputDevice for Master)
    int pendingFirstChannel { -1 };            // routing request that must be retried after a device-list rebuild
    int pendingNumChannels  { 0 };

    // Level metering: registered client reads peak values each frame.
    // Holding a ref-counted pointer keeps the plugin (and its measurer) alive even if
    // the track is deleted before this strip is destroyed.
    juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> meterPlugin;
    te::LevelMeasurer::Client levelClient;

    // UI Components
    juce::Label nameLabel;
    juce::ComboBox inputSelector;
    juce::Slider volumeSlider;
    juce::Label volumeValueLabel;
    juce::TextButton muteButton;
    juce::TextButton soloButton;
    std::unique_ptr<VUMeter> vuMeter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackStrip)
};
