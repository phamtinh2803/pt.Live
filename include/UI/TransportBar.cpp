// NOT BUILT: legacy copy. The real TransportBar is src/UI/TransportBar.cpp - make every change THERE.
// This file can be deleted.
/*
    pt.Live - Live Singing DAW
    TransportBar implementation (bottom bar, spec section 11)
*/

#include "include/UI/TransportBar.h"
#include "include/UI/MasterRecorder.h"
#include "include/Theme.h"

using namespace ptl;

//==============================================================================
class TransportBar::RecButton : public juce::Button
{
public:
    RecButton() : juce::Button ("Record") { setClickingTogglesState (true); }

    void setBlinkLit (bool lit)
    {
        if (blinkLit != lit)
        {
            blinkLit = lit;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        using namespace theme;

        const bool recording = getToggleState();
        auto b = getLocalBounds().toFloat();

        g.setColour (down || (over && ! recording) ? raisedHi : raised);
        g.fillRoundedRectangle (b, radius);

        if (! recording)
        {
            g.setColour (outline);
            g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);
        }
        else
        {
            if (blinkLit)
            {
                g.setColour (danger.withAlpha (0.18f));
                g.fillRoundedRectangle (b, radius);
            }
            g.setColour (blinkLit ? dangerHover : danger.withAlpha (0.45f));
            g.drawRoundedRectangle (b.reduced (0.75f), radius, 1.5f);
        }

        const auto label = recording ? juce::String ("Recording") : juce::String ("Record");
        const auto f = font (szSmall, true);

        juce::GlyphArrangement ga;
        ga.addLineOfText (f, label, 0.0f, 0.0f);
        const float textW  = ga.getBoundingBox (0, -1, false).getWidth();
        const float glyphS = 9.0f, gap = 8.0f;
        const float total  = glyphS + gap + textW;
        const float x0     = b.getCentreX() - total * 0.5f;

        g.setColour (recording ? dangerHover : danger);
        if (recording)
            g.fillRect (x0, b.getCentreY() - glyphS * 0.5f, glyphS, glyphS);
        else
            g.fillEllipse (x0, b.getCentreY() - glyphS * 0.5f, glyphS, glyphS);

        g.setColour (over ? ptl::theme::text : ptl::theme::textDim);
        g.setFont (f);
        g.drawText (label, juce::Rectangle<float> (x0 + glyphS + gap, b.getY(), textW + 4.0f, b.getHeight()),
                    juce::Justification::centredLeft, false);
    }

private:
    bool blinkLit = true;
};

//==============================================================================
TransportBar::TransportBar()
    : recButton (std::make_unique<RecButton>())
{
    recButton->onClick = [this] { onRecordPressed(); };
    addAndMakeVisible (*recButton);
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

    for (auto* l : { &timecodeLabel, &formatLabel, &statusLabel })
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

    recButton->setBounds (centred (r.removeFromLeft (150), 150, 26));
    r.removeFromLeft (10);
    timecodeLabel.setBounds (centred (r.removeFromLeft (78), 78, 22));
    r.removeFromLeft (10);

    auto statusArea = r.removeFromRight (juce::jmin (420, getWidth() / 2));
    statusPill = centred (statusArea, statusArea.getWidth(), 22);
    statusLabel.setBounds (statusPill.reduced (4, 0));

    formatLabel.setBounds (centred (r, r.getWidth(), 22));
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
    // Recording blink: red when recording, pulsing every 400ms for visibility.
    bool recording = false;
    if (auto* rec = audioEngine != nullptr ? audioEngine->getMasterRecorder() : nullptr)
        recording = rec->isRecording();

    recButton->setBlinkLit (recording && (juce::Time::getMillisecondCounter() % 800 < 400));
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

    // Recording clock
    bool recording = false;
    if (auto* rec = audioEngine->getMasterRecorder())
    {
        recording = rec->isRecording();
        if (recording)
            lastRecordSeconds = rec->getDurationSeconds();
    }

    if (recButton->getToggleState() != recording)
        recButton->setToggleState (recording, juce::dontSendNotification);

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

    // Dropouts (callbacks longer than their block). Shown only once one has happened.
    if (const auto xr = audioEngine->getXRunCount(); xr > 0)
        s << "  |  XR " << juce::String ((int) juce::jmin (xr, 9999u));

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
    if (audioEngine == nullptr)
        return;

    auto* rec = audioEngine->getMasterRecorder();
    if (rec == nullptr)
        return;

    if (recButton->getToggleState())
    {
        // Master bus must be running to be recorded
        if (! audioEngine->isPlaying())
            audioEngine->play();

        // The recorder creates Desktop/Record and names the file with the recording
        // start time: yyyy-MM-dd_HH-mm-ss_zzz.wav.  Sample rate = the one currently
        // set in Devices (44.1 kHz or 48 kHz...).
        juce::String error;

        if (rec->beginRecording (error))
        {
            lastRecordSeconds = 0.0;
        }
        else
        {
            recButton->setToggleState (false, juce::dontSendNotification);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Record", error, "OK");
        }
    }
    else
    {
        rec->stopRecording();
    }

    updateStatus();
}
