/*
    pt.Live - Live Singing DAW
    VU Meter implementation

    Renders dual vertical LED-style meters with proper color zones
    and peak hold behavior.

    Range: [-72dB, 0dB] — extended from [-60dB, 0dB] so quiet signals
    don't light half the bars. Color thresholds stay broadcast-standard:
      green   < -12 dB
      yellow  -12 to -3 dB
      orange  -3 to -1 dB
      red     > -1 dB
*/

#include "include/UI/VUMeter.h"
#include "include/Theme.h"

using namespace ptl;

VUMeter::VUMeter()
{
    setSize(colWidth * 2 + gapPixels + 8, totalHeight);
    startTimerHz(meterHz);  // 20 Hz UI update
}

int VUMeter::calculateSegmentIndex(float dBFS) const
{
    // Map -72dB to 0dB onto [0, totalSegments) — extended low end for broadcast-standard behavior.
    // Small levels now sit in the lower-middle instead of lighting half the bars.
    const float dbRange = maxDb - minDb;  // 72 dB range
    float normalized = (dBFS - minDb) / dbRange;  // [0, 1]
    return juce::jlimit(0, totalSegments - 1,
                        static_cast<int>(normalized * (totalSegments - 1)));
}

juce::Colour VUMeter::getSegmentColor(int segmentIndex) const
{
    // Thresholds recalibrated for the [-72dB, 0dB] range (24 segments, 3 dB each):
    // segments 0-18  : green  (below -12 dB)
    // segments 19-21 : yellow (-12 dB to -3 dB)
    // segment  22    : orange (-3 dB to ~-1 dB)
    // segment  23    : red    (above ~-1 dB)

    if (segmentIndex <= 18)
        return theme::vuGreen;
    if (segmentIndex <= 21)
        return theme::vuYellow;
    if (segmentIndex <= 22)
        return theme::vuOrange;
    return theme::vuRed;
}

void VUMeter::setLeftLevel(float dBFS)
{
    leftLevel = juce::jlimit(minDb, maxDb, dBFS);

    // Peak holds monotonically upward
    if (leftLevel > peakLeft)
        peakLeft = leftLevel;
}

void VUMeter::setRightLevel(float dBFS)
{
    rightLevel = juce::jlimit(minDb, maxDb, dBFS);

    if (rightLevel > peakRight)
        peakRight = rightLevel;
}

void VUMeter::setCurrentLevels(float leftRMS, float rightRMS)
{
    setLeftLevel(leftRMS);
    setRightLevel(rightRMS);
}

void VUMeter::timerCallback()
{
    // Apply peak decay gradually
    if (! isShowing())          // minimised / hidden: nothing to draw, skip the work
        return;

    const float decayPerFrame = peakDecayRateDbPerSec / (float) meterHz;   // dB per timer tick

    bool needsRepaint = false;

    if (peakLeft > leftLevel)
    {
        peakLeft = juce::jmax(leftLevel, peakLeft - decayPerFrame);
        needsRepaint = true;
    }
    if (peakRight > rightLevel)
    {
        peakRight = juce::jmax(rightLevel, peakRight - decayPerFrame);
        needsRepaint = true;
    }

    if (needsRepaint)
        repaint();
}

void VUMeter::resized()
{
    // No dynamic layout needed - simple fixed-size component
}

void VUMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    auto bg = bounds.toFloat();

    g.setColour(theme::bgTertiary);
    g.fillRect(bg);

    // Draw left and right columns
    drawColumn(g, leftLevel, peakLeft, true);
    drawColumn(g, rightLevel, peakRight, false);
}

void VUMeter::drawColumn(juce::Graphics& g, float level, float peak, bool isLeft)
{
    auto bounds = getLocalBounds().toFloat().reduced(4.0f, 4.0f);
    const float pixelHeight = bounds.getHeight();

    const float xBase = isLeft
        ? bounds.getX()
        : bounds.getX() + static_cast<float>(colWidth) + static_cast<float>(gapPixels);

    // Calculate segment indices
    const int activeSeg = calculateSegmentIndex(level);
    const int peakSeg = calculateSegmentIndex(peak);

    // Segment height (no gaps between segments for the LED look, thin line separators)
    const float segHeight = pixelHeight / static_cast<float>(totalSegments);

    // Draw segments from bottom to top
    for (int seg = 0; seg < totalSegments; ++seg)
    {
        const float yFromBottom = static_cast<float>(seg) * segHeight;

        juce::Rectangle<float> segRect(
            xBase,
            bounds.getBottom() - yFromBottom - segHeight,
            static_cast<float>(colWidth),
            segHeight - 1.0f);   // 1px gap between segments

        const bool isActive = (seg <= activeSeg);
        const bool isPeakOnly = (! isActive && seg <= peakSeg);

        if (isActive)
        {
            // Solid fill for current level
            g.setColour(getSegmentColor(seg));
            g.fillRect(segRect);
        }
        else if (isPeakOnly)
        {
            // Draw faded outline for peak hold
            g.setColour(getSegmentColor(seg).withAlpha(0.5f));
            g.fillRect(segRect);
        }
        else
        {
            // Unlit segment: very dark
            g.setColour(theme::bgPrimary);
            g.fillRect(segRect);
        }
    }

    // Draw 1px separator between L/R (drawn once, on the left column's right edge)
    if (isLeft)
    {
        g.setColour(theme::borderDark);
        g.fillRect(juce::Rectangle<float>(bounds.getX() + static_cast<float>(colWidth),
                                          bounds.getY(),
                                          static_cast<float>(gapPixels),
                                          bounds.getHeight()));
    }
}
