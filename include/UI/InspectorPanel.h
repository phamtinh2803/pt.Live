/*
    pt.Live - Live Singing DAW
    InspectorPanel - left-side editor for the SELECTED track

        AUDIO INPUT · MONO          (header: kind + name)
        LEVEL    volume knob | pan knob
        INSERTS  VST3 plugins on the track  (+ Add insert)
        SENDS    sends to FX channels       (+ Add send)   [input tracks + FX channels]
                 every track with a SENDS card (input tracks AND FX channels) can also get one
                 "Master" send (Send Master, see TrackManager); an FX channel additionally
                 offers OTHER FX channels that do not create a loop

    Volume / pan are wired to the track's VolumeAndPan plugin (Master: master volume).
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/TrackManager.h"

class PluginScanner;

class InspectorPanel : public juce::Component
{
public:
    InspectorPanel(TrackManager& mgr, PluginScanner* scanner);
    ~InspectorPanel() override;

    void setSelectedTrack(te::Track* trk);   // nullptr -> empty state
    void refresh();                          // re-read everything from the selected track

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class Content;
    class InsertRow;
    class SendRow;

    void updateContentSize();

    std::unique_ptr<Content> content;
    juce::Viewport viewport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorPanel)
};
