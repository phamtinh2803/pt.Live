/*
    pt.Live - Live Singing DAW
    Audio Settings dialog — Cubase / Studio One style.

    Layout (top to bottom):

        Row 1 : ASIO Driver  (asio4all / asiolinkpro / soundcard name)
        Row 2 : Sample Rate | Buffer Size | Latency
        Row 3 : Control Panel                      [x] PDC
        Row 4 : INPUT CHANNELS (left)  |  OUTPUT CHANNELS (right)
        Row 5 :                                           [ Close ]

    The driver list shows real ASIO drivers only. Choosing a driver opens it;
    only then are its input/output channels queried and shown.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Theme.h"
#include "include/Audio/AudioEngine.h"

class AudioSettingsContent : public juce::Component,
                             private juce::ChangeListener,
                             private juce::Timer
{
public:
    // onPDCChanged: called after the PDC checkbox changed the engine (lets the main window refresh its indicators).
    explicit AudioSettingsContent (AudioEngine& engine, std::function<void (bool)> onPDCChanged = nullptr);
    ~AudioSettingsContent() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    juce::AudioDeviceManager& devMgr() const;

    void rebuildDriverList (bool rescan);
    void rebuildRateList();
    void rebuildBufferList();
    void rebuildChannelRows();
    void updateLatencyLabel();
    void layoutChannelRows();

    void onDriverChanged();
    void onRateChanged();
    void onBufferChanged();
    void toggleChannels (bool isInput, int firstChannel, int count, bool shouldBeOn);

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshFromEngine();
    void timerCallback() override;

    // Coming back from the driver's control panel: only re-read and show what the driver reports now.
    // The driver is NOT reloaded (the engine follows by itself when the driver asks for a reset).
    void refreshAfterPanel();

    // The driver's control panel is a separate window; when the app comes back to the front after
    // it was opened we follow the driver once, to pick up what was changed there.
    bool panelOpened   = false;
    bool wasForeground = true;

    // Values behind the combo items (the engine's config can change between two clicks).
    juce::Array<double> shownRates;
    juce::Array<int>    shownBuffers;

    // Which ASIO type owns each driver in the combo.
    juce::StringArray driverOwnerTypes;

    //=== Channel rows ======================================================
    struct ChannelRow : public juce::Component
    {
        std::function<void (bool)> onToggle;
        bool active = false;
        juce::String name;

        ChannelRow (const juce::String& n, bool isActive)
            : active (isActive), name (n)
        {
            setInterceptsMouseClicks (true, false);
        }

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds().reduced (8, 0);

            auto box = area.removeFromLeft (18).withSizeKeepingCentre (16, 16).toFloat();
            box = box.reduced (0.5f);

            if (active)
            {
                g.setColour (ptl::theme::accentGreen);
                g.fillRoundedRectangle (box, 3.0f);

                juce::Path tick;
                tick.startNewSubPath (box.getX() + box.getWidth() * 0.26f, box.getCentreY());
                tick.lineTo (box.getCentreX(), box.getBottom() - box.getHeight() * 0.28f);
                tick.lineTo (box.getRight() - box.getWidth() * 0.24f, box.getY() + box.getHeight() * 0.30f);
                g.setColour (ptl::theme::bgPrimary);
                g.strokePath (tick, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
            }
            else
            {
                g.setColour (ptl::theme::borderLight);
                g.drawRoundedRectangle (box, 3.0f, 1.2f);
            }

            area.removeFromLeft (8);
            g.setColour (active ? ptl::theme::textPrimary : ptl::theme::textDimmed);
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText (name, area, juce::Justification::centredLeft, true);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            active = ! active;
            if (onToggle)
                onToggle (active);
            repaint();
        }
    };

    juce::OwnedArray<ChannelRow> inputRows, outputRows;

    AudioEngine& audioEngine;

    //=== Controls ==========================================================
    juce::Label  driverCaption { "driver", "ASIO Driver" };
    juce::ComboBox driverBox;

    juce::Label  rateCaption   { "rate",   "Sample Rate" };
    juce::ComboBox rateBox;

    juce::Label  bufferCaption { "buffer", "Buffer Size" };
    juce::ComboBox bufferBox;

    juce::Label  latencyCaption { "latency", "Latency" };
    juce::Label  latencyValue   { "latencyValue", "-" };

    juce::TextButton panelBtn  { "Control Panel" };
    juce::ToggleButton pdcToggle { "PDC" };
    std::function<void (bool)> pdcChanged;
    juce::TextButton closeBtn  { "Close" };

    juce::Label inCaption      { "in",  "Input Channels" };
    juce::Label outCaption     { "out", "Output Channels" };
    juce::Viewport inViewport, outViewport;
    juce::Component inHolder, outHolder;     // owned by the viewports' view, NOT children of this component
    juce::Rectangle<int> inFrameRect, outFrameRect;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSettingsContent)
};

class AudioSettingsDialog
{
public:
    static void show (AudioEngine& engine, juce::Component* parent = nullptr,
                      std::function<void (bool)> onPDCChanged = nullptr);
};
