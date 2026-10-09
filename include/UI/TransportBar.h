/*
    pt.Live - Live Singing DAW
    Bottom bar (40px) - transport + device status      (spec section 11)

        [ ● Record ]  0:00:00   WAV 24-bit 44.1kHz            44.1 kHz | 448 smp | 11.2 ms | CPU 003%   v0.1.0

    pt.Live has NO timeline playback: monitoring starts with the app, and Record writes the
    MASTER output to a WAV file.  Monitor / LLM / Plugins / Devices moved to the title bar.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/AudioEngine.h"

class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    TransportBar();
    ~TransportBar() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

    // Attach the audio engine (owned by MainWindow)
    void setAudioEngine (AudioEngine* engine);

    // Status refresh (also driven by the internal 30 Hz timer)
    void updateStatus();

    // The Record button now lives on the ControlBar; the bottom bar only mirrors its state
    // (timecode + red 'recording' tint on the status area). Called by MainWindow.
    void setRecordingActive (bool on);

private:
    void timerCallback() override;
    void onRecordPressed();

    static juce::String formatTimecode (double seconds);

    juce::Label timecodeLabel, formatLabel, statusLabel, versionLabel;   // versionLabel: "v0.1.0", far right

    juce::Rectangle<int> statusPill;
    bool  recordingActive = false;                 // mirrored from the master recorder
    float cpuLoad = 0.0f;
    double lastRecordSeconds = 0.0;
    int blinkFrame = 0;

    AudioEngine* audioEngine = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};
