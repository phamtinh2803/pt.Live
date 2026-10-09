/*
    pt.Live - Live Singing DAW
    TransportBar implementation (bottom bar, spec section 11)
*/

#include "include/UI/TransportBar.h"
#include "include/UI/MasterRecorder.h"
#include "include/Theme.h"

using namespace ptl;

//==============================================================================
TransportBar::TransportBar()
{
    setOpaque(true);  // P0-3: reduce repaints

    timecodeLabel.setFont (theme::monoFont (theme::szBody));
    timecodeLabel.setColour (juce::Label::textColourId, theme::text);
    timecodeLabel.setJustificationType (juce::Justification::centredLeft);

    formatLabel.setFont (theme::font (theme::szMicro));
    formatLabel.setColour (juce::Label::textColourId, theme::textDim);
    formatLabel.setJustificationType (juce::Justification::centredLeft);

    statusLabel.setFont (theme::font (theme::szSmall));
    statusLabel.setColour (juce::Label::textColourId, theme::textFaint);
    statusLabel.setJustificationType (juce::Justification::centredRight);

    // App version (single source of truth: MainApplication::getApplicationVersion() <- VERSION in CMakeLists.txt)
    {
        juce::String ver;
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            ver = app->getApplicationVersion();

        versionLabel.setText (ver.isNotEmpty() ? "v" + ver : juce::String(), juce::dontSendNotification);
        versionLabel.setFont (theme::font (theme::szMicro));
        versionLabel.setColour (juce::Label::textColourId, theme::textFaint);
        versionLabel.setJustificationType (juce::Justification::centredRight);
    }

    for (auto* l : { &timecodeLabel, &formatLabel, &statusLabel, &versionLabel })
    {
        l->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
    }

    timecodeLabel.setText ("0:00:00", juce::dontSendNotification);
    // P0-3: Reduce to 10 Hz for status updates, record blink still at 15/15 @ 30Hz
    startTimer(100);  // 10 Hz for timecode/format/status
}

TransportBar::~TransportBar() = default;

void TransportBar::setAudioEngine (AudioEngine* engine)
{
    audioEngine = engine;
    updateStatus();
}

void TransportBar::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (1);
    r = r.reduced (12, 0);

    auto centred = [] (juce::Rectangle<int> area, int w, int h) { return area.withSizeKeepingCentre (w, h); };

    // No more record button - moved to ControlBar
    timecodeLabel.setBounds (centred (r.removeFromLeft (100), 100, 22));
    r.removeFromLeft (15);

    formatLabel.setBounds (centred (r.removeFromLeft (140), 140, 22));
    r.removeFromLeft (15);

    versionLabel.setBounds (centred (r.removeFromRight (56), 56, 22));     // far right: "v0.1.0"
    r.removeFromRight (10);

    auto statusArea = r.removeFromRight (juce::jmin (380, getWidth() / 2));
    statusPill = centred (statusArea, statusArea.getWidth(), 22);
    statusLabel.setBounds (statusPill.reduced (4, 0));
}

void TransportBar::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (window);
    g.setColour (hairline);
    g.fillRect (0, 0, getWidth(), 1);

    // CPU wash behind the status text: grows left -> right with the load
    if (! statusPill.isEmpty())
    {
        const float frac = juce::jlimit (0.0f, 1.0f, cpuLoad);
        const auto col = frac < 0.7f ? accent : frac < 0.9f ? warning : danger;

        auto pill = statusPill.toFloat();
        const float w = pill.getWidth() * frac;

        if (w > 0.5f)
        {
            g.saveState();
            juce::Path clip;
            clip.addRoundedRectangle (pill, radius);
            g.reduceClipRegion (clip);

            g.setColour (col.withAlpha (0.22f));
            g.fillRect (pill.withWidth (w));
            g.setColour (col.withAlpha (0.65f));
            g.fillRect (pill.getX() + w - 1.5f, pill.getY(), 1.5f, pill.getHeight());
            g.restoreState();
        }
    }
}

void TransportBar::timerCallback()
{
    // Mirror the current recorder state and repaint the status pill red when recording.
    bool recording = false;
    if (auto* rec = audioEngine != nullptr ? audioEngine->getMasterRecorder() : nullptr)
        recording = rec->isRecording();

    if (recording != recordingActive)
    {
        recordingActive = recording;
        repaint (statusPill);
    }

    // ~400ms pulse rate while recording -> clearly visible without being distracting.
    // Outside the timer: timecode/format/status refresh at 10Hz (100ms intervals).
    updateStatus();
}

juce::String TransportBar::formatTimecode (double seconds)
{
    const int total = juce::jmax (0, (int) seconds);
    const int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    return juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s).paddedLeft ('0', 2);
}

void TransportBar::updateStatus()
{
    if (audioEngine == nullptr || ! audioEngine->isReady())
    {
        statusLabel.setText ("Audio engine not ready", juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, theme::textFaint);
        return;
    }

    const double sr = audioEngine->getSampleRate();
    const bool hasDevice = sr > 0.0;

    // Recording clock: driven by the current master recorder state.
    const auto* rec = audioEngine ? audioEngine->getMasterRecorder() : nullptr;
    if (rec)
    {
        if (rec->isRecording())
            lastRecordSeconds = rec->getDurationSeconds();
        else if (! rec->isRecording() && recordingActive)
            recordingActive = false;    // stop was called externally
    }

    timecodeLabel.setText (formatTimecode (lastRecordSeconds), juce::dontSendNotification);

    // "WAV 24-bit 44.1kHz"
    formatLabel.setText ("WAV 24-bit " + juce::String (sr / 1000.0, 1) + "kHz", juce::dontSendNotification);

    // "44.1 kHz  |  448 smp  |  11.2 ms  |  CPU 003%"   (CPU padded: fixed width, no layout jitter)
    const float cpu = audioEngine->getCpuUsage();
    const int cpuPct = juce::jlimit (0, 999, juce::roundToInt (cpu * 100.0f));

    juce::String s;
    s << juce::String (sr / 1000.0, 1) << " kHz  |  "
      << audioEngine->getBlockSize() << " smp  |  "
      << juce::String (audioEngine->getTotalLatencyMs(), 1) << " ms  |  CPU "
      << juce::String (cpuPct).paddedLeft ('0', 3) << "%";

    if (statusLabel.getText() != s)
        statusLabel.setText (s, juce::dontSendNotification);

    statusLabel.setColour (juce::Label::textColourId, hasDevice ? theme::text : theme::textFaint);

    if (std::abs (cpu - cpuLoad) > 0.005f)
    {
        cpuLoad = cpu;
        repaint (statusPill);
    }
}

void TransportBar::onRecordPressed()
{
    // Record is now handled by the ControlBar (MainWindow drives the recorder). The bottom
    // bar only mirrors its state, so this hook is intentionally left as a no-op; MainWindow
    // calls the recorder directly.
    juce::ignoreUnused (this);
}

void TransportBar::setRecordingActive (bool on)
{
    if (recordingActive == on)
        return;
    recordingActive = on;
    if (! on)
        lastRecordSeconds = 0.0;    // a fresh take starts at 0
    repaint (statusPill);
}
