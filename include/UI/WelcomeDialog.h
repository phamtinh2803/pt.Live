/*
    pt.Live - Live Singing DAW
    Start screen - shown when pt.Live is started directly (the .exe), before the main window exists.

        [ animated pt.Live brand ]                                              [x]
        ------------------------------------------------------------------------
        Welcome to pt.Live
        Live Singing DAW
        Open a saved session (.pt), or start with an empty session.

        [   Open...   ]   [    Empty    ]   [    Cancel    ]

        RECENT
        [ file name                                              date  time ]
        [ folder                                                            ]     <- the 3 newest .pt files
        ...                                                                         (section hidden when there are none)

    Enter = Open..., Esc / [x] / Cancel = quit pt.Live. The result is reported once, from a clean message-loop turn,
    after the window has closed. Borderless window in the same style as LicenseDialog.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Theme.h"
#include "include/TitleBar.h"
#include "include/UI/UIWidgets.h"

class WelcomeDialog : public juce::Component
{
public:
    enum class Choice { open, empty, recent, cancel };

    // `file` is only filled for Choice::recent.
    using Chosen = std::function<void (Choice choice, const juce::File& file)>;

    static void show (const juce::Array<juce::File>& recentFiles, Chosen onChosen);

    WelcomeDialog (const juce::Array<juce::File>& recentFiles, Chosen onChosen);
    ~WelcomeDialog() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;

    void requestCancel();                             // Cancel / [x] / Esc

    static constexpr int dialogWidth = 520;

private:
    class RecentRow;

    void choose (Choice c, const juce::File& file = {});

    Chosen onChosen;
    bool finished = false;
    juce::Array<juce::File> recentFiles;

    BrandMark brand;
    juce::TextButton openBtn, emptyBtn, cancelBtn;
    juce::OwnedArray<RecentRow> rows;
    juce::ComponentDragger dragger;

    juce::Rectangle<int> headerR, headlineR, subR, explainR, recentLabelR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WelcomeDialog)
};
