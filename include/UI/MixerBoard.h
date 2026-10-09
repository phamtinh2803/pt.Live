/*
    pt.Live - Live Singing DAW
    MixerBoard - track list panel: a vertical stack of LANES (spec section 6)

        STEREO INPUT  (stereo input rows, "+" adds a stereo input)
        MONO INPUT    (mono input rows, "+" adds a mono input)
        FX CHANNELS   ("+" adds an FX channel)
        STEREO OUT    (Master, always present, no "+")

    Each lane = tinted panel + raised header band + rule + subtle left rail.
    Groups are told apart by STRUCTURE, never by colour.
*/

#pragma once

#include <JuceHeader.h>
#include <map>
#include "include/Audio/TrackManager.h"
#include "include/UI/TrackRow.h"
#include "include/UI/UIWidgets.h"
#include "include/UI/VUMeter.h"        // VUMeter::meterHz (shared meter refresh rate)

class MixerBoard : public juce::Component,
                   private juce::Timer
{
public:
    explicit MixerBoard(TrackManager& mgr);
    ~MixerBoard() override { trackManager.onGraphRebuilt = nullptr; }

    // Rebuild every lane/row from the current Edit state.
    void syncWithTrackManager();

    void setDeviceManager(te::DeviceManager* dm) { deviceManager = dm; }

    // Low-latency monitoring toggled (or plugins changed): re-evaluate the red "heavy plugin" borders now.
    void refreshHeavyIndicators()
    {
        for (auto* lane : lanes)
            for (auto* row : lane->rows)
                row->refreshHeavyState();
    }

    // Selection (highlights the row). notify = also fire onTrackSelected.
    void selectTrack(te::Track* track, bool notify);
    te::Track* getSelectedTrack() const noexcept { return selectedTrack; }

    void resized() override;
    void paint(juce::Graphics& g) override;

    // Shared timer for level updates at 30 Hz
    void startLevelTimer() { startTimerHz(VUMeter::meterHz); }
    void stopLevelTimer() { stopTimer(); }

    std::function<void(te::Track*)> onTrackSelected;
    std::function<void(te::Track*)> onTrackRenamed;
    std::function<void(te::Track*)> onDeleteRequested;
    std::function<void(te::Track*)> onTrackInputChanged;     // user changed the input of a track
    std::function<void(TrackManager::TrackType)> onAddTrackRequested;
    std::function<void(float, float)> onMasterLevels;            // dB L/R of the master bus @ 30 Hz

private:
    //==========================================================================
    // One lane = tinted panel + raised header band + rule + left rail, holding TrackRows.
    class LanePanel : public juce::Component
    {
    public:
        static constexpr int headerH = 30, rowH = 58, rowGap = 4, padBottom = 6;

        LanePanel(const juce::String& titleText, const juce::String& nounText, juce::Colour edgeColour, bool showAddButton)
            : title(titleText), noun(nounText), edge(edgeColour),
              addButton(ptl::icons::Id::Plus)
        {
            addButton.setTooltip("Add " + noun);
            addButton.onClick = [this] { if (onAdd) onAdd(); };
            addChildComponent(addButton);
            addButton.setVisible(showAddButton);
        }

        TrackRow* addRow(std::unique_ptr<TrackRow> row)
        {
            auto* r = rows.add(row.release());
            addAndMakeVisible(r);
            return r;
        }

        int getRequiredHeight() const
        {
            if (collapsed)
                return headerH + 1;                            // only the header band is left

            const int n = rows.size();
            return headerH + 1 + juce::jmax(1, n) * (rowH + rowGap) + padBottom;
        }

        // One click on the header band collapses / expands the lane (no button needed).
        // The "+" button is a child component and keeps its own clicks.
        void setCollapsed(bool shouldBeCollapsed)
        {
            collapsed = shouldBeCollapsed;

            for (auto* r : rows)
                r->setVisible(! collapsed);

            repaint();
        }

        bool isCollapsed() const noexcept { return collapsed; }

        void mouseMove(const juce::MouseEvent& e) override
        {
            const bool over = e.y < headerH;
            setMouseCursor(over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);

            if (over != headerHover)
            {
                headerHover = over;
                repaint();
            }
        }

        void mouseExit(const juce::MouseEvent&) override
        {
            if (headerHover)
            {
                headerHover = false;
                repaint();
            }
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (e.y >= headerH || e.mods.isPopupMenu() || e.mouseWasDraggedSinceMouseDown())
                return;

            setCollapsed(! collapsed);

            if (onCollapsedChanged)
                onCollapsedChanged(collapsed);
        }

        void paint(juce::Graphics& g) override
        {
            using namespace ptl;
            auto b = getLocalBounds().toFloat();

            g.setColour(theme::laneFill);
            g.fillRoundedRectangle(b, theme::radiusLg);

            auto header = b.removeFromTop((float) headerH);
            g.setColour(theme::raised);

            if (collapsed)
            {
                g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::radiusLg);   // header only: all corners round
            }
            else
            {
                g.fillRoundedRectangle(header.withHeight(header.getHeight() + 4.0f), theme::radiusLg);
                g.fillRect(header.withTrimmedTop(header.getHeight() * 0.5f));

                g.setColour(theme::outline);                   // rule under the header
                g.fillRect(0.0f, (float) headerH, (float) getWidth(), 1.0f);
            }

            if (headerHover)                                   // hover feedback: this band is clickable
            {
                g.setColour(juce::Colours::white.withAlpha(0.05f));
                g.fillRoundedRectangle(juce::Rectangle<float>(0.0f, 0.0f, (float) getWidth(), (float) headerH), theme::radiusLg);
            }

            // Left rail: a quiet 2px tint (was solid white = harsh). Clipped to the lane's rounded corners.
            {
                juce::Graphics::ScopedSaveState state(g);
                juce::Path lanePath;
                lanePath.addRoundedRectangle(getLocalBounds().toFloat(), theme::radiusLg);
                g.reduceClipRegion(lanePath);

                g.setColour(edge.withAlpha(0.22f));
                g.fillRect(0.0f, 0.0f, 2.0f, (float) getHeight());
            }

            g.setColour(theme::sectionLabel);
            g.setFont(theme::spacedFont(theme::szSmall, 0.12f, true));
            g.drawText(title, 14, 0, getWidth() - 50, headerH, juce::Justification::centredLeft, true);

            if (rows.isEmpty() && ! collapsed)
            {
                g.setColour(theme::textFaint);
                g.setFont(theme::font(theme::szMicro));
                g.drawText("No " + noun + " yet", 14, headerH + 1, getWidth() - 28, rowH,
                           juce::Justification::centredLeft, true);
            }
        }

        void resized() override
        {
            addButton.setBounds(getWidth() - 28, (headerH - 22) / 2, 22, 22);

            int y = headerH + 1 + rowGap;
            for (auto* r : rows)
            {
                r->setBounds(6, y, getWidth() - 12, rowH);
                y += rowH + rowGap;
            }
        }

        std::function<void()> onAdd;
        std::function<void(bool)> onCollapsedChanged;       // user clicked the header; arg = now collapsed
        juce::OwnedArray<TrackRow> rows;

    private:
        bool collapsed = false;
        bool headerHover = false;
        juce::String title, noun;
        juce::Colour edge;
        ptl::ui::IconButton addButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LanePanel)
    };

    void timerCallback() override;

    LanePanel* addLane(const juce::String& title, const juce::String& noun, juce::Colour edge, bool showAdd,
                       const juce::Array<te::Track*>& tracks, std::function<void()> onAdd);
    void layoutLanes();

    TrackManager& trackManager;
    te::DeviceManager* deviceManager { nullptr };
    te::Track* selectedTrack { nullptr };

    // Collapsed state per lane title: survives syncWithTrackManager() (which rebuilds every lane
    // whenever a track is added / removed).
    std::map<juce::String, bool> collapsedLanes;

    juce::Component content;
    juce::OwnedArray<LanePanel> lanes;
    juce::Viewport viewport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerBoard)
};
