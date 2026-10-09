/*
    pt.Live - Live Singing DAW
    VUMeter - Vertical dual-channel LED bar meter

    Features:
    - Left/Right channel columns with 1px gap
    - Color zones: green (-12dB) -> yellow (-3dB) -> orange (-1dB) -> red (>0dB)
    - Peak hold functionality with decay
    - Timer-based updates at 30Hz
    - High-performance rendering
*/

#pragma once

#include <JuceHeader.h>

class VUMeter : public juce::Component,
                private juce::Timer
{
public:
    VUMeter();
    ~VUMeter() = default;

    // Level update (called every ~50ms from timerCallback)
    void setLeftLevel(float dBFS);       // range: -60.0 to 0.0
    void setRightLevel(float dBFS);

    // Current levels for fill bars
    void setCurrentLevels(float leftRMS, float rightRMS);

    // Resize aware layout calculation
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Start/stop update timer
    // Meter refresh rate shared by every meter timer (20 Hz is smooth enough for LEDs and ~1/3 cheaper than 30 Hz).
    static constexpr int meterHz = 20;

    void startUpdating() { startTimerHz(meterHz); }
    void stopUpdating() { stopTimer(); }

private:
    // Current fill levels (RMS)
    float leftLevel { -72.0f };
    float rightLevel { -72.0f };

    // Peak hold values
    float peakLeft { -72.0f };
    float peakRight { -72.0f };

    // Decay rate (dB per second)
    static constexpr float peakDecayRateDbPerSec = 2.0f;

    // Layout constants
    static constexpr int totalHeight = 180;      // scalable
    static constexpr int colWidth = 28;          // per column
    static constexpr int gapPixels = 1;          // mandatory 1px gap
    static constexpr int totalSegments = 24;     // L/R each
    static constexpr float minDb = -72.0f;       // bottom (extended for better visibility of quiet signals)
    static constexpr float maxDb = 0.0f;         // top

    // Calculate which segment is lit for a given dB value
    int calculateSegmentIndex(float dBFS) const;

    // Segment colors by threshold
    juce::Colour getSegmentColor(int segmentIndex) const;

    // Draw helper
    void drawColumn(juce::Graphics& g, float level, float peak, bool isLeft);

    // Timer callback
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VUMeter)
};
