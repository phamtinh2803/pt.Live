/*
    pt.Live - Live Singing DAW
    TrackRow - one horizontal mixer card (58px, spec section 6.3):

      [icon] NAME            [input combo] > [output combo] [trash] [mute] [solo]
      ▬▬▬▬ meter 80x5

    Used for Stereo In / Mono In / FX channel / Master rows.
      - Input tracks : input combo (ASIO channel / channel pair) + fixed "MASTER" output
      - FX channels  : no input combo (they are fed by sends)
      - Master       : only the hardware OUTPUT combo
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/TrackManager.h"
#include "include/UI/UIWidgets.h"

class TrackRow : public juce::Component,
                 private juce::ChangeListener
{
public:
    TrackRow(TrackManager& mgr, te::Track* trk);
    ~TrackRow() override;

    te::Track* getTrack() const noexcept { return track; }

    // The playback graph was re-created (plugin bus layout change): route the saved input again.
    void reapplyInputRouting();

    // Lets the row list real ASIO channels / outputs and follow driver changes.
    void setDeviceManager(te::DeviceManager* dm);
    void setSelected(bool shouldBeSelected);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override { updateHover(); }
    void mouseExit(const juce::MouseEvent&) override  { updateHover(); }

    // Master row only: fed with the post-fader levels (dB) at 30 Hz for the big LED meter.
    std::function<void(float, float)> onLevels;

    // Pull the current meter level and forward it to the strip. Called by MixerBoard's shared timer.
    void updateMeter();

    // Low-latency monitoring: red border while this track holds a heavy plugin (never bypasses it).
    void refreshHeavyState();

    std::function<void(te::Track*)> onSelected;
    std::function<void(te::Track*)> onDeleteRequested;
    std::function<void(te::Track*)> onRenamed;
    std::function<void(te::Track*)> onInputChangedByUser;   // the user picked another input in the combo

private:
    void updateHover();
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    // Level meter (audio track OR master bus)
    void attachLevelMeter();
    void detachLevelMeter();
    juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> findOrCreateLevelMeter();

    // Combos
    void populateSelectors();
    void populateInputSelector();
    void updateInputWarning();                // "No Input" = amber warning state of the input combo
    void populateOutputSelector();            // Master: hardware output (headphones / monitors)
    void handleInputSelectionChanged();
    void handleOutputSelectionChanged();

    // Input routing helpers (physical wave input -> this track)
    bool isStereoTrack() const { return kind == TrackManager::TrackType::StereoIn; }
    bool routeInputChannels(int firstChannel, int numChannels);
    void clearTrackRouting(te::AudioTrack&);
    bool routeTrackTo(te::AudioTrack&, te::WaveInputDevice&);
    bool isRoutedFromAnyInput() const;
    void restoreSavedRouting();               // re-applies the input (tracks) / output (Master) the user chose and saved
    TrackManager& trackManager;
    te::Track* track { nullptr };
    TrackManager::TrackType kind;
    te::DeviceManager* deviceManager { nullptr };
    bool selected { false };
    bool hover { false };
    bool heavy { false };                     // low-latency monitoring ON + a heavy plugin on this track
    int  heavyTick { 0 };
    float lrPeakL { -100.0f }, lrPeakR { -100.0f };   // diagnostics: MONO IN left/right peak between two log checks
    juce::uint32 lrLastCheckMs { 0 };
    bool lrWasMismatch { false };
    int  lastLoggedMeterChans { -1 };         // diagnostics: logs when the meter sees 1 <-> 2 channels
    float masterMuteRestoreDb { 0.0f };       // Master "mute" = fader pulled down, restored on un-mute

    static constexpr int noInputItemId     = 1;
    static constexpr int placeholderItemId = 2;
    static constexpr int firstDeviceItemId = 100;
    juce::StringArray outputDeviceIDs;         // Master combo: index n -> WaveOutputDevice::getDeviceID()
    int pendingFirstChannel { -1 };            // routing request to retry after a device-list rebuild
    int pendingNumChannels  { 0 };
    int restoreTries        { 0 };             // attempts to restore the saved routing since it was last found routed
    juce::int64 lastRestoreOkMs { 0 };         // when a restore last succeeded: both change broadcasters fire back to back, route only once

    juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> meterPlugin;
    te::LevelMeasurer::Client levelClient;

    juce::Label nameLabel;
    juce::ComboBox inputSelector;
    juce::Label arrowLabel;
    juce::ComboBox outputSelector;
    ptl::ui::IconButton deleteButton;
    ptl::ui::IconButton muteButton;
    ptl::ui::IconButton soloButton;
    ptl::ui::VuStrip levelBar;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackRow)
};
