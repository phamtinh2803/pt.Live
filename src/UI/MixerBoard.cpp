/*
    pt.Live - Live Singing DAW
    MixerBoard implementation - lanes of TrackRows.
*/

#include "include/UI/MixerBoard.h"
#include "include/Theme.h"

using namespace ptl;

MixerBoard::MixerBoard(TrackManager& mgr)
    : trackManager(mgr)
{
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);                 // vertical scroll only
    viewport.setScrollBarThickness(10);
    addAndMakeVisible(viewport);
    setOpaque(true);  // P0-3: reduce repaints by not redrawing parent when transparent

    startLevelTimer();  // Start shared timer instead of each TrackRow

    // After a plugin layout change the playback graph is re-created and loses every input route.
    trackManager.onGraphRebuilt = [this] (te::Track* t)
    {
        for (auto* lane : lanes)
            for (auto* row : lane->rows)
                if (t == nullptr || row->getTrack() == t)
                    row->reapplyInputRouting();
    };
}

MixerBoard::LanePanel* MixerBoard::addLane(const juce::String& title, const juce::String& noun, juce::Colour edge,
                                           bool showAdd, const juce::Array<te::Track*>& tracks,
                                           std::function<void()> onAdd)
{
    auto* lane = lanes.add(new LanePanel(title, noun, edge, showAdd));
    content.addAndMakeVisible(lane);
    lane->onAdd = std::move(onAdd);

    for (auto* t : tracks)
    {
        if (t == nullptr)
            continue;

        auto* row = lane->addRow(std::make_unique<TrackRow>(trackManager, t));

        row->onSelected = [this](te::Track* tr) { selectTrack(tr, true); };
        row->onRenamed  = [this](te::Track* tr) { if (onTrackRenamed) onTrackRenamed(tr); };
        row->onDeleteRequested = [this](te::Track* tr) { if (onDeleteRequested) onDeleteRequested(tr); };
        row->onInputChangedByUser = [this](te::Track* tr) { if (onTrackInputChanged) onTrackInputChanged(tr); };

        if (trackManager.getTrackType(t) == TrackManager::TrackType::Master)
            row->onLevels = [this](float l, float r) { if (onMasterLevels) onMasterLevels(l, r); };

        row->setDeviceManager(deviceManager);
        row->setSelected(t == selectedTrack);
    }

    // Collapse state is remembered per lane; one click on the header toggles it.
    if (const auto it = collapsedLanes.find(title); it != collapsedLanes.end() && it->second)
        lane->setCollapsed(true);

    lane->onCollapsedChanged = [this, title](bool nowCollapsed)
    {
        collapsedLanes[title] = nowCollapsed;
        layoutLanes();
        repaint();
    };

    return lane;
}

void MixerBoard::syncWithTrackManager()
{
    lanes.clear();                                            // child components remove themselves

    using TT = TrackManager::TrackType;

    // STEREO INPUT and MONO INPUT are two separate lanes, each with its own "+".
    juce::Array<te::Track*> stereoIn, monoIn, fx, master;
    for (auto* t : trackManager.getTracksOfType(TT::StereoIn))  stereoIn.add(t);
    for (auto* t : trackManager.getTracksOfType(TT::MonoIn))    monoIn.add(t);
    for (auto* t : trackManager.getTracksOfType(TT::FXChannel)) fx.add(t);
    if (auto* m = trackManager.getMasterTrack())
        master.add(m);

    juce::Component::SafePointer<MixerBoard> safe(this);

    addLane("STEREO INPUT", "stereo input", theme::laneAudio, true, stereoIn,
            [safe] { if (safe != nullptr && safe->onAddTrackRequested) safe->onAddTrackRequested(TT::StereoIn); });

    addLane("MONO INPUT", "mono input", theme::laneAudio, true, monoIn,
            [safe] { if (safe != nullptr && safe->onAddTrackRequested) safe->onAddTrackRequested(TT::MonoIn); });

    addLane("FX CHANNELS", "FX channel", theme::laneFx, true, fx,
            [safe] { if (safe != nullptr && safe->onAddTrackRequested) safe->onAddTrackRequested(TT::FXChannel); });

    addLane("STEREO OUT", "output", theme::laneOut, false, master, {});

    layoutLanes();
    repaint();
}

void MixerBoard::selectTrack(te::Track* track, bool notify)
{
    selectedTrack = track;

    for (auto* l : lanes)
        for (auto* r : l->rows)
            r->setSelected(r->getTrack() == track);

    if (notify && onTrackSelected)
        onTrackSelected(track);
}

void MixerBoard::layoutLanes()
{
    const int contentW = juce::jmax(300, viewport.getMaximumVisibleWidth());
    const int laneX = 2;
    const int laneW = contentW - 16 - laneX;                  // 16px breathing room on the right

    int y = 8;
    for (auto* l : lanes)
    {
        const int h = l->getRequiredHeight();
        l->setBounds(laneX, y, laneW, h);
        y += h + 14;                                          // 14px between lanes
    }

    content.setSize(contentW, y);
}

void MixerBoard::resized()
{
    viewport.setBounds(getLocalBounds());
    layoutLanes();
}

void MixerBoard::timerCallback()
{
    if (! isShowing())          // window minimised / mixer hidden: stop polling the meters
        return;

    // Shared 20Hz timer: pull levels from each TrackRow's meter and repaint only
    // the ones whose level actually changed (VuStrip::setLevels already guards this).
    for (auto* lane : lanes)
        for (auto* row : lane->rows)
            row->updateMeter();
}

void MixerBoard::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    g.setColour(theme::hairline);
    g.fillRect(0, 0, 1, getHeight());                         // divider against the inspector
}
