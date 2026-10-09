/*
    pt.Live - Live Singing DAW
    ControlBar - the live control panel, right above the bottom bar.

        <title of the YouTube video, letters drop in; long titles then scroll>                          [DANH SÁCH]
        TRACK                                  TONE                 KEY
        [BEAT][MICRO][VANG] [ C# MINOR ]  [HA] 0 [NANG]   [-] 0 [+]   [TOGGLEn ...]        [RECORD 0:00:00]
        BEAT -o------ 0.0 dB   MICRO -o------ 0.0 dB   RETUNE --o---- 20   FLEX o------- 0   [FX]

    - BEAT / MICRO : on-off of the track (mute).   VANG : on-off of all FX sends of MICRO.
                     While one of them is OFF its border blinks red every 500 ms.
    - Record       : far right, alone, with its own red look and divider (never blue, so it is not mistaken for BEAT / MICRO).
                     The clock of the take sits INSIDE the button; while recording the whole button pulses solid red every 500 ms.
    - KEY / SCALE  : highlighted box between VANG and TONE (click = pick a key by hand).
    - TONE / KEY   : HA / NANG / - / + use the same look as BEAT / MICRO / VANG when ON
                     (bright blue border, bright text), with larger text.
    - TONE         : real pitch shift of the music (SoundShifter Pitch). Moves the Autotune key with it.
    - KEY          : only moves the key given to Autotune, the music pitch stays.
    - Key readout  : the key Autotune really got. Click it to pick a key by hand (when the VPS failed).
    - [+]          : ONE "+" right of KEY. Its menu has SLIDER and TOGGLE. Any number of both.
    - Sliders      : BEAT / MICRO fader (double-click = 0 dB), RETUNE SPEED and FLEX TUNE of the Autotune on MICRO
                     (the number shown is the one Autotune itself shows). Sliders the user adds go in rows BELOW these
                     (the panel gets taller by one row each time the row is full).
                     [+] > Slider > <track> > <VST> > search + pick one parameter  (or  <track> > "send level" of an FX).
                     The slider shows the parameter's own name and value text, dragging it moves the VST right now.
                     Double-click the NAME (or right-click) = rename, right-click = remove.
    - Toggles      : [+] > Toggle > <function> > <track> [> <VST / send>].  ONE button = ONE function, lit = ON:
                     track on/off (mute), track solo, one VST on/off, all INSERTS of a track on/off, all SENDS of a
                     track on/off, one send on/off, PDC. The first 4 sit in the top row right of the "+", more go in
                     the rows below with the sliders. Click = on/off, double-click = rename, right-click = rename / remove.
                     Everything is saved inside the session (.pt).
    - ALL EFFECT   : a TOGGLE function ([+] > Toggle > ALL EFFECT). Lit = every INSERT and SEND of every track (Master
                     too) runs; dark = all of them bypassed, so the sound going in = the sound coming out. It uses the
                     same bypass as the inspector's INSERTS / SENDS buttons, so switching back restores exactly what was on.
    - Placeholders : a NEW session starts with 2 empty TOGGLEs and 4 empty SLIDERs (nothing assigned, hollow dot / "—").
                     Click one (or right-click > "Gán chức năng...") to give it a function; right-click > "Xoá" removes it.
    - Text size    : BEAT / MICRO / VANG / RECORD use the same text size as HA / NANG (the reference). HA and NANG
                     buttons are always the same width.
    - Master meter : while the panel is collapsed ([^]) the same meter as the main app's master meter (on its own black plate) sits at the far
                     right edge (the mixer's own master meter is hidden then). Everything else shifts left to make room.
    - Title strip  : title of the song being sung, shown from its first LETTER (leading digits, brackets,
                     time stamps ... are cut). While the VPS looks for the key a bar runs under it and the key
                     box shows a spinner "Đang dò key / scale" (purely visual: the buttons are NOT locked).
    - [^] / [v]    : collapse pt.Live to just this panel (the window drops to the bottom edge of the screen,
                     above the taskbar) / bring everything back.
    Tone and key steps show "cung" like ptStudio did (one step = 1 semitone = 0.5 cung).

    The panel only shows what LiveController says (it polls ~7 times a second), so it always agrees
    with the mixer even when a track is muted from the mixer itself.
*/

#pragma once

#include <JuceHeader.h>
#include <vector>
#include "include/Theme.h"
#include "include/Audio/LiveController.h"

class ControlBar : public juce::Component,
                   public juce::TooltipClient,          // full status text when the mouse is over the key box
                   private juce::Timer
{
public:
    enum class StatusKind { Idle, Info, Ok, Warn, Error };

    explicit ControlBar (LiveController& controller);
    ~ControlBar() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    juce::String getTooltip() override;

    void setStatus (const juce::String& text, StatusKind kind);
    // A failed lookup: short text in the key box + a "THỬ LẠI" button (onRetry). `detail` is the full text (tooltip).
    void setFailure (const juce::String& shortText, const juce::String& detail);
    void flashResult();                  // a key was just found: the key text blinks for 2 s
    // exact title of the focused YouTube video ("" = none). skipped = not a karaoke / beat video: shown as "[Bỏ qua] <title>".
    void setSongTitle (const juce::String& title, bool skipped = false);
    void setLoading (bool on);                       // true while the key / scale of the new song is being looked up
    void setCompact (bool compact);               // only changes the chevron direction
    void refresh();                               // re-read the controller now
    juce::Rectangle<int> getHistoryButtonScreenBounds() const;   // where the DANH SÁCH popup points to

    // Width the panel needs to show everything with the toggles it has right now (used by the window as its
    // minimum width). It grows when a TOGGLE is added and shrinks when one is removed.
    int getIdealWidth() const;
    // Height for a given width: the base panel plus one row for every full row of user-made sliders / toggles.
    int getIdealHeightForWidth (int width) const;
    std::function<void()> onIdealWidthChanged;    // a slider / toggle was added / removed (or the rows changed) -> the needed size changed

    std::function<void()> onRetry;                // the user pressed THỬ LẠI after a failed key lookup
    std::function<void()> onToggleCompact;
    std::function<void()> onShowHistory;          // DANH SÁCH button -> show history popup
    std::function<void()> onRecordPressed;        // Record button pressed
    std::function<bool()> isRecording;            // polled: true while the master recorder is running
    std::function<double()> getRecordSeconds;     // polled: length of the take so far (shown INSIDE the RECORD button)

private:
    class PanelButton;
    class ParamSlider;
    class MarqueeLabel;
    class MasterMeter;

    void timerCallback() override;
    void showKeyMenu();

    // User-made sliders / toggles (see LiveController::CtlItem)
    struct ItemUi
    {
        int id = 0;
        LiveController::CtlItem::Kind kind = LiveController::CtlItem::Kind::Toggle;
        std::unique_ptr<ParamSlider> slider;       // Kind::ParamSlider / SendSlider
        std::unique_ptr<PanelButton> button;       // Kind::Toggle
        juce::String tip;
    };
    struct ExtraLayout
    {
        int rows = 0;                                    // ALL rows from tier 2 down (slider grid + toggle rows), 0 = none
        int gridRows = 0;                                // rows of the slider grid
        std::vector<juce::Rectangle<int>> rects;         // rects[i]: for itemUis[i] (slider or toggle), empty = in tier 1
    };                                                   // all rects are relative to the top of tier 2

    void syncItems();                                  // creates / removes the components when the list of items changed
    void updateItems();                                // polls the live state of every item
    ExtraLayout computeExtras (int width) const;       // layout of ALL sliders (grid) + the toggles that did not fit in tier 1
    std::vector<std::pair<ParamSlider*, int>> gridSliders() const;   // the user-made sliders (index in itemUis)
    bool inTier1 (size_t index) const;                 // is itemUis[index] one of the first toggles (top row)?
    int  numTier1Toggles() const;
    void showAddMenu (int assignId = -1);              // the "+": SLIDER / TOGGLE, then track, VST ...  assignId >= 0: fills that empty item instead
    void pickParameter (const juce::String& trackId, const juce::String& pluginId, const juce::String& pluginName, int assignId = -1);
    void showItemMenu (int id);                        // right-click: rename / remove
    void beginRename (int id);
    void commitRename (bool apply);
    void closeRenameEditor();                          // removes the editor without applying anything
    juce::Rectangle<int> readoutBounds() const;
    juce::Rectangle<int> retryPillBounds() const;
    static juce::String formatSteps (int steps);

    LiveController& live;

    std::vector<ItemUi> itemUis;                    // user-made sliders / toggles, in the controller's order
    juce::String itemsSignature;                    // ids + kinds the components were built for
    int extraRows = 0;                              // rows of sliders / toggles under tier 1 for the current width
    int gridRows = 0, toggleRows = 0;
    int lastReportedHeight = ptl::theme::controlBarH;   // last panel height the window was told about
    std::unique_ptr<MasterMeter> masterMeter;       // vertical master VU meter, far right, only in the collapsed panel
    int meterReserve() const;                       // width kept free on the right for it (0 when it is hidden)
    std::unique_ptr<PanelButton> beatBtn, micBtn, vangBtn, toneDownBtn, toneUpBtn, keyDownBtn, keyUpBtn, compactBtn;
    std::unique_ptr<PanelButton> danhSachBtn, recBtn, addBtn;      // addBtn: the single "+" = menu: add a SLIDER or a TOGGLE

    juce::Rectangle<int> trackGroup, toneGroup, keyGroup, recGroup, toneValue, keyValue, readout, titleStrip, titleText;
    juce::uint32 marqueeStartMs = 0;                // title animation clock: restarts on a new song
    juce::uint32 flashStartMs = 0;                  // key text blink (2 s) after a result
    bool flashing = false;
    juce::String prevTitle;                         // old title, flying out while the new one flies in
    juce::uint32 outStartMs = 0;                    // clock of the fly-out animation
    static constexpr float outMs = 900.0f;          // duration of the fly-out phase (old title)
    int titleW = 0;                                 // pixel width of the current title (set while painting)
    int tier1DividerX[4] { 0, 0, 0, 0 };            // [3] = divider in front of RECORD

    std::unique_ptr<juce::TextEditor> renameEditor;     // inline editor over a toggle button / slider name
    int renameId = -1;                                  // id of the item being renamed

    juce::String songTitle;
    bool songSkipped = false, prevSkipped = false;    // "[Bỏ qua]" titles are drawn dimmer than a real song
    bool loading = false;
    int  tick = 0;

    juce::String statusText;
    juce::String detailText;                        // long text for the tooltip (failure details)
    bool retryAvailable = false;                    // show the THỬ LẠI button in the key box
    bool overRetry = false;
    StatusKind statusKind = StatusKind::Idle;
    juce::String shownKey;
    juce::String recTimeText { "0:00:00" };   // clock of the take, drawn above the RECORD button
    bool recClockLive = false;                 // true while recording (clock drawn bright)
    bool compactMode = false;
    bool overReadout = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlBar)
};
