/*
    pt.Live - Live Singing DAW
    ControlBar implementation.
*/

#include "include/UI/ControlBar.h"
#include "include/UI/VUMeter.h"          // VUMeter::meterHz
#include "include/UI/UIWidgets.h"        // ptl::ui::MasterMeter (the main app's master meter)

using namespace ptl;

//==============================================================================
// Flat button in the same language as the rest of the app (raised surface, hairline outline,
// hue only for state).
class ControlBar::PanelButton : public juce::Button,
                                private juce::Timer
{
public:
    // Toggle : dot + label, lit blue while ON (BEAT / MICRO / VANG / TOGGLEn)
    // Step   : no state, always drawn like a lit Toggle (HA / NANG / - / +)
    enum class Kind { Toggle, Step, Action, Chevron, Record };

    PanelButton (const juce::String& text, Kind k) : juce::Button (text), kind (k)
    {
        if (kind == Kind::Toggle)
            setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    ~PanelButton() override { stopTimer(); }

    void setPointsUp (bool up) { if (pointsUp != up) { pointsUp = up; repaint(); } }

    // "Nothing assigned": the state dot is drawn as an empty ring and the label is dimmed.
    void setHollow (bool h) { if (hollow != h) { hollow = h; repaint(); } }

    void setFontSize (float h) { if (fontSize != h) { fontSize = h; repaint(); } }

    // Action button that must catch the eye (DANH SACH): accent-tinted body, bright border, list icon before the label.
    void setProminent (bool p) { if (prominent != p) { prominent = p; repaint(); } }

    // While the button is OFF (and usable) its border blinks red, 500 ms on / 500 ms off.
    void setBlinkWhenOff (bool b)
    {
        if (blinkWhenOff == b) return;
        blinkWhenOff = b;
        blinkPhase = false;
        if (b) startTimer (500); else if (! recording) stopTimer();
        repaint();
    }

    // Record button: always drawn like a lit control (blue border, like BEAT / MICRO / VANG when ON).
    // Only while RECORDING does its border flash red, 500 ms on / 500 ms off.
    void setRecording (bool r)
    {
        if (recording == r) return;
        recording = r;
        blinkPhase = false;
        if (r) startTimer (500); else if (! blinkWhenOff) stopTimer();
        repaint();
    }

    // Double-click is used for something else (rename): the 2nd click of a double-click must not
    // toggle the button a second time.
    void setDoubleClickSwallowsClick (bool b) { swallowDoubleClick = b; }

    // Narrow button (BEAT / MICRO / VANG / Record): smaller state dot, tighter margins, text squeezed instead of cut.
    void setTight (bool t) { tight = t; }

    std::function<void()> onRightClick;
    std::function<void()> onDoubleClick;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
        juce::Button::mouseDown (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;

        if (swallowDoubleClick && e.getNumberOfClicks() >= 2)
        {
            setState (isMouseOver() ? buttonOver : buttonNormal);       // no click for the 2nd press
            return;
        }
        juce::Button::mouseUp (e);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && onDoubleClick)
            onDoubleClick();
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        using namespace theme;

        const bool en     = isEnabled();
        const bool toggle = kind == Kind::Toggle;
        const bool on     = toggle && getToggleState();
        const bool step   = kind == Kind::Step;                         // HA / NANG / - / +: always drawn "lit", quietly
        const bool isRec  = kind == Kind::Record;

        auto b = getLocalBounds().toFloat().reduced (0.5f);
        auto fill = raised, border = outline, txt = textDim;
        float borderW = 1.0f;
        bool  gradientOn = false;
        float softTop = 0.0f, softBot = 0.0f;                           // > 0: soft vertical gradient (Step / prominent Action)
        auto softCol = accent;

        // OFF + blinking: the border flashes red.
        const bool blinkNow = en && blinkPhase
                           && ((toggle && blinkWhenOff && ! on && ! hollow) || (isRec && recording));

        if (! en)
        {
            fill = raised.withAlpha (0.5f);
            txt  = textFaint;
        }
        else if (isRec)
        {
            // RECORD has its own look (red, never blue): idle = red ring on a dark body, recording = solid red pulse.
            const auto red = (over || down) ? dangerHover : recordRed;
            if (recording)
            {
                fill    = recordRed.withAlpha (blinkPhase ? 0.95f : 0.45f);
                border  = dangerHover;
                borderW = 1.8f;
                txt     = theme::textOnAccent;
            }
            else
            {
                fill    = stage.overlaidWith (recordRed.withAlpha (down ? 0.34f : over ? 0.24f : 0.12f));
                border  = red.withAlpha (0.85f);
                borderW = 1.4f;
                txt     = theme::text;
            }
        }
        else if (on)
        {
            // ON: unmistakable. Solid accent body (gradient), bright border, white text, glowing dot.
            gradientOn = true;
            fill    = accent.withAlpha (down ? 0.78f : over ? 0.66f : 0.56f);      // alpha = strength of the gradient below
            border  = accent;
            borderW = 1.4f;
            txt     = theme::text;
        }
        else if (step)
        {
            // HA / NANG / - / +: exactly the look of BEAT / MICRO / VANG when ON (same blue body, border and text).
            gradientOn = true;
            fill    = accent.withAlpha (down ? 0.78f : over ? 0.66f : 0.56f);
            border  = accent;
            borderW = 1.4f;
            txt     = theme::text;
        }
        else if (prominent && kind == Kind::Action)
        {
            softTop = down ? 0.38f : over ? 0.30f : 0.22f;
            softBot = down ? 0.28f : over ? 0.20f : 0.12f;
            fill    = accent.withAlpha (softBot);
            border  = (over || down) ? accentHover : accent.withAlpha (0.65f);
            borderW = 1.2f;
            txt     = theme::text;
        }
        else if (toggle && ! hollow)
        {
            // OFF: recessed, dark, text dimmed, thin red-tinted rim ("this sound is not running").
            fill   = down ? raisedHi : over ? raised.brighter (0.06f) : stage;
            border = danger.withAlpha (over ? 0.85f : 0.55f);
            txt    = over ? theme::text : textDim;
        }
        else if (down) { fill = raisedHi.brighter (0.08f); txt = theme::text; }
        else if (over) { fill = raisedHi; txt = theme::text; border = outlineStrong; }

        if (blinkNow && ! isRec)
        {
            border  = danger;
            fill    = danger.withAlpha (0.14f);
            borderW = 1.8f;
        }

        if (gradientOn && ! blinkNow)
        {
            juce::ColourGradient grad (accent.withAlpha (fill.getFloatAlpha()), 0.0f, b.getY(),
                                       accentDeep.withAlpha (juce::jmin (1.0f, fill.getFloatAlpha() + 0.1f)), 0.0f, b.getBottom(), false);
            g.setGradientFill (grad);
        }
        else if (softTop > 0.0f && ! blinkNow)
        {
            g.setGradientFill (juce::ColourGradient (softCol.withAlpha (softTop), 0.0f, b.getY(),
                                                     softCol.withAlpha (softBot), 0.0f, b.getBottom(), false));
        }
        else
            g.setColour (fill);
        g.fillRoundedRectangle (b, radius);

        g.setColour (border);
        g.drawRoundedRectangle (b.reduced (borderW > 1.0f ? 0.4f : 0.0f), radius, borderW);

        if (kind == Kind::Chevron)
        {
            const auto c = b.getCentre();
            const float w = 6.0f, h = 3.0f, dir = pointsUp ? -1.0f : 1.0f;     // up = "^" (collapse), down = "v" (expand)
            juce::Path p;
            p.startNewSubPath (c.x - w, c.y - dir * h);
            p.lineTo (c.x, c.y + dir * h);
            p.lineTo (c.x + w, c.y - dir * h);
            g.setColour (txt);
            g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            return;
        }

        // ---- Label (+ optional leading dot / icon), laid out as ONE group centred in the button ----------
        // The dot / icon sits right before the text and is centred on the CAP HEIGHT of the text (not on the
        // font's line box, which is what made the dots look too low), so dot and letters line up exactly.
        const bool hasDot  = toggle || isRec;
        const bool hasIcon = prominent && kind == Kind::Action;
        const float lead    = hasDot ? (tight ? 8.0f : (fontSize >= 14.0f ? 10.0f : 8.0f)) : hasIcon ? 14.0f : 0.0f;
        const float leadGap = lead > 0.0f ? (tight ? 7.0f : 8.0f) : 0.0f;
        const float margin  = hasDot ? (tight ? 8.0f : 10.0f) : 6.0f;
        const float availW  = juce::jmax (4.0f, b.getWidth() - 2.0f * margin);
        const float maxTextW = juce::jmax (4.0f, availW - lead - leadGap);
        const auto  label   = getButtonText();

        juce::GlyphArrangement ga;
        auto layoutText = [&] (float h)
        {
            ga.clear();
            ga.addLineOfText (font (h, true), label, 0.0f, 0.0f);
            return ga.getBoundingBox (0, -1, true).getWidth();
        };

        float h = fontSize;
        float textW = layoutText (h);
        if (tight && textW > maxTextW)                                   // squeezed (down to 70 %) instead of cut
        {
            h = fontSize * juce::jmax (0.7f, maxTextW / textW);
            textW = layoutText (h);
        }
        if (textW > maxTextW)                                            // still too wide: ellipsis
        {
            ga.clear();
            ga.addCurtailedLineOfText (font (h, true), label, 0.0f, 0.0f, maxTextW, true);
            textW = juce::jmin (maxTextW, ga.getBoundingBox (0, -1, true).getWidth());
        }

        // Cap height of this font: tight outline of the letter "H" (baseline = 0, so its top is at -capH).
        float capH = h * 0.55f;
        {
            juce::GlyphArrangement hg;
            hg.addLineOfText (font (h, true), "H", 0.0f, 0.0f);
            juce::Path hp;
            hg.createPath (hp);
            if (! hp.isEmpty())
                capH = juce::jmax (1.0f, -hp.getBounds().getY());
        }

        const float cy       = b.getCentreY();
        const float baseline = cy + capH * 0.5f;
        const float groupW   = lead + leadGap + textW;
        const float startX   = groupW <= availW ? b.getCentreX() - groupW * 0.5f : b.getX() + margin;
        const float textX    = startX + lead + leadGap;

        if (hasDot)
        {
            // State dot.  Toggle ON = bright dot with a glow, OFF = hollow red ring, nothing assigned = faint ring.
            // Record: red dot (solid white-red while recording).
            const float d = lead;
            const float x = startX;
            const float y = cy - d * 0.5f;

            if (hollow)
            {
                g.setColour (textFaint);
                g.drawEllipse (x + 0.5f, y + 0.5f, d - 1.0f, d - 1.0f, 1.2f);
                txt = textFaint;
            }
            else if (! en)
            {
                g.setColour (textFaint);
                g.fillEllipse (x, y, d, d);
            }
            else if (isRec)
            {
                g.setColour (recording ? theme::textOnAccent : dangerHover);
                g.fillEllipse (x, y, d, d);
            }
            else if (on)
            {
                g.setColour (theme::textOnAccent.withAlpha (0.18f));
                g.fillEllipse (x - 3.0f, y - 3.0f, d + 6.0f, d + 6.0f);        // glow
                g.setColour (theme::textOnAccent);
                g.fillEllipse (x, y, d, d);
            }
            else
            {
                g.setColour (danger);
                g.drawEllipse (x + 0.7f, y + 0.7f, d - 1.4f, d - 1.4f, 1.6f);    // hollow ring = OFF
            }
        }
        else if (hasIcon)
        {
            // "List" icon: three bars (the middle one a little shorter) = the song list.
            g.setColour (txt);
            const float iw = lead, bh = 2.0f, barStep = 4.0f;
            const float iy = cy - barStep - bh * 0.5f;
            g.fillRoundedRectangle (startX, iy,            iw,        bh, 1.0f);
            g.fillRoundedRectangle (startX, iy + barStep,     iw * 0.72f, bh, 1.0f);
            g.fillRoundedRectangle (startX, iy + 2 * barStep, iw,        bh, 1.0f);
        }

        g.setColour (txt);
        ga.draw (g, juce::AffineTransform::translation (textX, baseline));
    }

private:
    void timerCallback() override
    {
        blinkPhase = ! blinkPhase;
        if (isEnabled() && ((kind == Kind::Toggle && ! getToggleState() && ! hollow)
                            || (kind == Kind::Record && recording)))
            repaint();
        else if (! blinkPhase)
            repaint();                      // make sure a stale red frame is cleared
    }

    Kind kind;
    bool pointsUp = true;
    bool tight = false;
    bool hollow = false;
    float fontSize = theme::szSmall;
    bool prominent = false;
    bool blinkWhenOff = false, blinkPhase = false;
    bool recording = false;
    bool swallowDoubleClick = false;
};

//==============================================================================
namespace
{
    constexpr int kSliderValueW = 64;
    constexpr float kCtlFont  = 15.5f;     // HA / NANG
    constexpr float kTrackFont = kCtlFont; // BEAT / MICRO / VANG / Record: SAME size as HA / NANG (the reference)
    constexpr int   kTrackBtnW = 82;       // BEAT / MICRO / VANG (wide enough for the bigger text)
    constexpr int   kRecBtnW   = 112;      // RECORD (dot + label; the take's clock is drawn above it, in the caption slot)
    constexpr int   kRecGap    = 26;       // clear space between the performance controls (BEAT / MICRO / KEY / toggles) and RECORD
    constexpr float kCaptionFont = 12.5f;  // TRACK / TONE / KEY captions
    constexpr float kLabelFont   = 12.5f;  // slider name
    constexpr float kValueFont   = 13.0f;  // slider value
    constexpr float kTitleFont   = 16.0f;  // song title strip
    constexpr int   kToneBtnMinW = 50;     // HA / NANG: both buttons always the SAME width
    constexpr int   kCellPad  = 6;         // inner padding of a slider cell
    constexpr float kSignFont = 26.0f;     // - / +

    constexpr int kToggleW = 76, kToggleGap = 4;                  // TOGGLE buttons of the top row
    constexpr int kTitleBtnW = 28, kTitleBtnH = 22, kTitleBtnGap = 6;   // [+] and [^] in the title row, next to DANH SÁCH
    constexpr int kMeterW = 26, kMeterPad = 8;                    // master VU meter of the collapsed panel (far right)
    constexpr int kMinTrackW  = 40;                               // shortest draggable line of a slider (label / value columns come on top)

    // User-made sliders / toggles: rows below the built-in sliders.
    constexpr int kMaxTier1Toggles = 4;                           // the first toggles sit in the top row, the others go in the rows
    constexpr int kExtraRowH       = 30;                          // height of one row (same as the built-in slider row)
    constexpr int kExtraGap        = 8;
    constexpr int kExtraToggleW    = 84;
    constexpr int kExtraLabelMax   = 120;                         // longest name column of a user-made slider (full name is in the tooltip)
    constexpr int kExtraValueW     = 74;

    // Slider grid: 1-5 sliders = ONE row (they shrink to fit), 6 = 3+3, 7 = 4+3, 8 = 4+4, 9 = 3+3+3 ...
    int desiredSliderRows (int n) { return n <= 0 ? 0 : n <= 5 ? 1 : (n + 3) / 4; }

    std::vector<int> splitSliderRows (int n, int rows)            // contiguous chunks, the first rows get the extra one
    {
        std::vector<int> counts;
        if (rows <= 0) return counts;
        const int base = n / rows, rem = n % rows;
        for (int r = 0; r < rows; ++r)
            counts.push_back (base + (r < rem ? 1 : 0));
        return counts;
    }

    int sliderGap (int /*totalN*/, int /*j*/) { return kExtraGap; }

    // Narrowest width of `cnt` sliders (starting at `from`) in one row: fixed parts + minimal lines + gaps.
    int sliderRowMinWidth (const std::vector<int>& fixed, int from, int cnt, int totalN)
    {
        int w = 0;
        for (int j = 0; j < cnt; ++j)
            w += fixed[(size_t) (from + j)] + kMinTrackW + (j > 0 ? sliderGap (totalN, j) : 0);
        return w;
    }

    // HA and NANG share ONE width: at least kToneBtnMinW, wider if the text of NANG needs it.
    int toneButtonWidth()
    {
        static int cached = 0;                     // the font never changes: measure the text once
        if (cached > 0) return cached;

        juce::GlyphArrangement ga;
        ga.addLineOfText (theme::font (kCtlFont, true), juce::String::fromUTF8 ("N\xC3\x82NG"), 0.0f, 0.0f);
        const int need = (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 20;   // text + 2 x 6 px inner margin + air (never clipped)
        return cached = juce::jmax (kToneBtnMinW, need);
    }

    // Width of the "[toggles of the top row][+]" group for `n` toggles in it.
    int toggleGroupWidth (int n)
    {
        return n * kToggleW + juce::jmax (0, n - 1) * kToggleGap;
    }
}

//==============================================================================
// Thin horizontal slider: LABEL  ----o----  value text. Drag, mouse wheel, double-click = default.
class ControlBar::ParamSlider : public juce::Component,
                                public juce::SettableTooltipClient          // setTooltip()
{
public:
    explicit ParamSlider (const juce::String& labelText) : label (labelText)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        updateLabelWidth();
    }

    void setLabel (const juce::String& t)
    {
        if (t == label) return;
        label = t;
        updateLabelWidth();
        repaint();
    }

    const juce::String& getLabel() const { return label; }

    // User-made sliders: longer names allowed, and a click / double-click on the NAME never moves the value
    // (double-click on the name = rename).
    void setUserMade (int maxLabelWidth)
    {
        userMade = true;
        maxLabelW = maxLabelWidth;
        updateLabelWidth();
    }

    std::function<void()> onRightClick;                     // right-click on the slider (e.g. change / remove)
    std::function<void()> onLabelDoubleClick;               // user-made sliders: double-click on the name
    std::function<void()> onClickUnavailable;               // left click while nothing is assigned / the target is gone

    // Value column width (short numbers like RETUNE / FLEX need much less than "-12.5 dB").
    void setValueWidth (int w) { valueW = w; repaint(); }

    // Everything of the slider that is NOT its draggable line: label + value columns + paddings.
    int getFixedWidth() const { return labelW + valueW + 2 * kCellPad + 12; }

    std::function<void (double)>         onUserChange;     // value 0..1, called while the user moves it
    std::function<juce::String (double)> formatValue;      // text for a value 0..1
    double defaultValue = -1.0;                            // double-click target (< 0 = none)
    double wheelStep    = 0.02;

    // From the controller (polling). Ignored while the user is dragging.
    void setValue (double v)
    {
        if (dragging) return;
        v = juce::jlimit (0.0, 1.0, v);
        if (std::abs (v - value) > 1.0e-4) { value = v; repaint(); }
    }

    void setAvailable (bool a)
    {
        if (a == available) return;
        available = a;
        setEnabled (a);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        // Soft recessed cell: shows where one slider ends and the next one begins (kept very quiet).
        const auto cell = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (stage.withAlpha (available ? 0.65f : 0.35f));
        g.fillRoundedRectangle (cell, 4.0f);
        g.setColour (hairline);
        g.drawRoundedRectangle (cell, 4.0f, 1.0f);

        auto row     = getLocalBounds().reduced (kCellPad, 0);
        auto labelR  = row.removeFromLeft (labelW);
        auto valueR  = row.removeFromRight (valueW);
        auto trackR  = row.reduced (6, 0);

        g.setColour (available ? textMid : textDim);
        g.setFont (spacedFont (kLabelFont, 0.08f, true));
        g.drawText (label, labelR, juce::Justification::centredLeft, true);

        const float ty = (float) trackR.getCentreY();
        const float x0 = (float) trackR.getX(), x1 = (float) trackR.getRight();
        const float tx = x0 + (x1 - x0) * (float) value;

        g.setColour (stage);
        g.fillRoundedRectangle (x0, ty - 2.5f, x1 - x0, 5.0f, 2.5f);
        g.setColour (hairline);
        g.drawRoundedRectangle (x0 + 0.5f, ty - 2.0f, x1 - x0 - 1.0f, 4.0f, 2.0f, 1.0f);

        if (available)
        {
            g.setColour (accent.withAlpha (hover || dragging ? 0.9f : 0.6f));
            g.fillRoundedRectangle (x0, ty - 2.5f, juce::jmax (0.0f, tx - x0), 5.0f, 2.5f);
        }

        g.setColour (! available ? textFaint : dragging ? accentHover : theme::text);
        g.fillEllipse (tx - 6.0f, ty - 6.0f, 12.0f, 12.0f);

        g.setColour (! available ? textDim : (dragging || hover) ? theme::text : textMid);
        g.setFont (font (kValueFont, true));
        g.drawText (available && formatValue ? formatValue (value) : juce::String (juce::CharPointer_UTF8 ("\xE2\x80\x94")),
                    valueR, juce::Justification::centredRight, false);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown  (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
        if (! available)                                   // empty / lost target: a click opens its menu, nothing is dragged
        {
            if (! (userMade && onLabel (e)) && onClickUnavailable) onClickUnavailable();
            return;
        }
        if (userMade && onLabel (e)) return;               // the name is not part of the draggable line
        dragging = true;
        moveTo (e);
    }
    void mouseDrag  (const juce::MouseEvent& e) override { moveTo (e); }
    void mouseUp    (const juce::MouseEvent&) override { dragging = false; repaint(); }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (userMade && onLabel (e))
        {
            if (onLabelDoubleClick) onLabelDoubleClick();
            return;
        }
        if (defaultValue < 0.0) return;
        value = defaultValue;
        if (onUserChange) onUserChange (value);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (w.deltaY == 0.0f) return;
        value = juce::jlimit (0.0, 1.0, value + (w.deltaY > 0.0f ? wheelStep : -wheelStep));
        if (onUserChange) onUserChange (value);
        repaint();
    }

private:
    bool onLabel (const juce::MouseEvent& e) const { return e.x < kCellPad + labelW; }

    // The label column is only as wide as its own text, so the slider starts right next to it.
    void updateLabelWidth()
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (theme::spacedFont (kLabelFont, 0.08f, true), label, 0.0f, 0.0f);
        labelW = juce::jmin (maxLabelW, (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 4);
    }

    void moveTo (const juce::MouseEvent& e)
    {
        auto trackR = getLocalBounds().reduced (kCellPad, 0).withTrimmedLeft (labelW).withTrimmedRight (valueW).reduced (6, 0);
        value = juce::jlimit (0.0, 1.0, (double) (e.x - trackR.getX()) / (double) juce::jmax (1, trackR.getWidth()));
        if (onUserChange) onUserChange (value);
        repaint();
    }

    juce::String label;
    int labelW = 40;
    int maxLabelW = 72;
    bool userMade = false;
    int valueW = kSliderValueW;
    double value = 0.0;
    bool available = true, hover = false, dragging = false;
};

//==============================================================================
// Searchable list of EVERY parameter a VST publishes (some have hundreds): type to filter (all words must
// match, any order), click a row or press Enter to choose, Up / Down move, Esc closes.
class ParamPicker : public juce::Component,
                    private juce::ListBoxModel
{
public:
    ParamPicker (juce::Array<LiveController::ParamInfo> params, const juce::String& pluginName,
                 std::function<void (const LiveController::ParamInfo&)> onPick)
        : all (std::move (params)), pick (std::move (onPick))
    {
        using namespace theme;

        titleLabel.setText (pluginName + juce::String::fromUTF8 (" — chọn parameter"), juce::dontSendNotification);
        titleLabel.setFont (font (szBody, true));
        titleLabel.setColour (juce::Label::textColourId, text);
        addAndMakeVisible (titleLabel);

        search.setMultiLine (false);
        search.setReturnKeyStartsNewLine (false);
        search.setScrollbarsShown (false);
        search.setFont (font (szBody, false));
        search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Gõ để tìm parameter..."), textFaint);
        search.setColour (juce::TextEditor::backgroundColourId,      stage);
        search.setColour (juce::TextEditor::textColourId,            text);
        search.setColour (juce::TextEditor::highlightColourId,       accent.withAlpha (0.45f));
        search.setColour (juce::TextEditor::highlightedTextColourId, text);
        search.setColour (juce::TextEditor::outlineColourId,         hairline);
        search.setColour (juce::TextEditor::focusedOutlineColourId,  accent);
        search.onTextChange = [this] { applyFilter(); };
        search.onReturnKey  = [this] { choose (list.getSelectedRow() >= 0 ? list.getSelectedRow() : 0); };
        search.onEscapeKey  = [this] { close(); };
        search.onKey = [this] (const juce::KeyPress& k)
        {
            if (k == juce::KeyPress::downKey)  { moveSelection (+1); return true; }
            if (k == juce::KeyPress::upKey)    { moveSelection (-1); return true; }
            return false;
        };
        addAndMakeVisible (search);

        countLabel.setFont (font (szMicro, false));
        countLabel.setColour (juce::Label::textColourId, textDim);
        addAndMakeVisible (countLabel);

        list.setRowHeight (24);
        list.setColour (juce::ListBox::backgroundColourId, stage);
        list.setColour (juce::ListBox::outlineColourId, hairline);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        applyFilter();
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        titleLabel.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        search.setBounds (r.removeFromTop (28));
        countLabel.setBounds (r.removeFromTop (20));
        list.setBounds (r);
    }

    void parentHierarchyChanged() override
    {
        if (getParentComponent() == nullptr || focused) return;
        focused = true;
        juce::Component::SafePointer<ParamPicker> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->search.grabKeyboardFocus(); });
    }

private:
    struct SearchBox : juce::TextEditor
    {
        std::function<bool (const juce::KeyPress&)> onKey;
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (onKey && onKey (k)) return true;
            return juce::TextEditor::keyPressed (k);
        }
    };

    static juce::String shownName (const LiveController::ParamInfo& p)
    {
        return p.name.isNotEmpty() ? p.name : "Parameter " + p.id;
    }

    void applyFilter()
    {
        shown.clear();
        const auto words = juce::StringArray::fromTokens (search.getText().trim().toLowerCase(), " ", "");

        for (int i = 0; i < all.size(); ++i)
        {
            const auto n = shownName (all.getReference (i)).toLowerCase();
            bool ok = true;
            for (auto& w : words)
                if (! n.contains (w)) { ok = false; break; }
            if (ok) shown.add (i);
        }

        countLabel.setText (juce::String (shown.size()) + " / " + juce::String (all.size()) + " parameter",
                            juce::dontSendNotification);
        list.updateContent();
        if (shown.isEmpty()) list.deselectAllRows(); else list.selectRow (0);
        list.repaint();
    }

    void moveSelection (int delta)
    {
        if (shown.isEmpty()) return;
        list.selectRow (juce::jlimit (0, shown.size() - 1, juce::jmax (0, list.getSelectedRow()) + delta));
    }

    void close()
    {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();                                  // deletes this component: touch nothing after it
    }

    void choose (int row)
    {
        if (row < 0 || row >= shown.size()) return;
        const auto info = all.getReference (shown[row]);     // copy: `this` is deleted by close()
        auto cb = pick;
        if (cb) cb (info);
        close();
    }

    // ListBoxModel
    int getNumRows() override { return shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= shown.size()) return;
        const auto& p = all.getReference (shown[row]);

        if (selected)
        {
            g.setColour (theme::accent.withAlpha (0.22f));
            g.fillRect (0, 0, w, h);
        }

        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (8, 0);
        auto valueR = r.removeFromRight (juce::jmin (110, r.getWidth() / 3));

        g.setColour (theme::text);
        g.setFont (theme::font (theme::szSmall, false));
        g.drawText (shownName (p), r, juce::Justification::centredLeft, true);

        g.setColour (theme::textDim);
        g.setFont (theme::monoFont (theme::szSmall));
        g.drawText (p.text, valueR, juce::Justification::centredRight, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu()) choose (row);
    }

    juce::Array<LiveController::ParamInfo> all;
    juce::Array<int> shown;                                  // indexes into `all` that match the search
    std::function<void (const LiveController::ParamInfo&)> pick;
    juce::Label titleLabel, countLabel;
    SearchBox search;
    juce::ListBox list { "params", this };
    bool focused = false;
};

//==============================================================================
// MASTER meter of the collapsed panel (far right). It is the SAME meter as the main app's master meter
// (ptl::ui::MasterMeter: same LEDs, colours, peak hold), standing on its own BLACK plate so it does not blend with
// the panel's other backgrounds. Own 20 Hz timer, does nothing while hidden (full app) or minimised. It reads the
// level meter plugin the mixer's master meter uses (LiveController::getMasterLevelMeter).
class ControlBar::MasterMeter : public juce::Component,
                                private juce::Timer
{
public:
    explicit MasterMeter (LiveController& lc) : live (lc)
    {
        setInterceptsMouseClicks (false, false);
        addAndMakeVisible (meter);
        startTimerHz (VUMeter::meterHz);
    }

    ~MasterMeter() override
    {
        stopTimer();
        detach();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);                         // own plate: pure black, square, flush with the panel's edges
    }

    void resized() override
    {
        meter.setBounds (getLocalBounds().reduced (0, 4));        // the meter centres its two columns by itself
    }

private:
    void attach()
    {
        detach();
        plugin = live.getMasterLevelMeter();
        if (plugin != nullptr)
            plugin->measurer.addClient (client);
    }

    void detach()
    {
        if (plugin != nullptr)
        {
            plugin->measurer.removeClient (client);
            plugin = nullptr;
        }
    }

    void timerCallback() override
    {
        if (! isShowing())                       // full app (hidden) or minimised: no work at all
            return;

        if (plugin == nullptr)
            attach();
        if (plugin == nullptr)
            return;

        float in[2] = { -100.0f, -100.0f };
        const int n = client.getNumChannelsUsed();
        for (int c = 0; c < juce::jmin (2, n); ++c)
            in[c] = client.getAndClearAudioLevel (c).dB;
        if (n == 1) in[1] = in[0];

        meter.setLevels (in[0], in[1]);
    }

    LiveController& live;
    juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> plugin;
    te::LevelMeasurer::Client client;
    ptl::ui::MasterMeter meter;
};

//==============================================================================
ControlBar::ControlBar (LiveController& controller) : live (controller)
{
    using K = PanelButton::Kind;
    auto make = [this] (const juce::String& txt, K k)
    {
        auto b = std::make_unique<PanelButton> (txt, k);
        addAndMakeVisible (*b);
        return b;
    };

    // Vietnamese labels are written as UTF-8 escapes so the file does not depend on the compiler's source charset.
    const auto ha   = juce::String::fromUTF8 ("H\xE1\xBA\xA0");                 // HẠ
    const auto nang = juce::String::fromUTF8 ("N\xC3\x82NG");                   // NÂNG

    beatBtn     = make ("BEAT", K::Toggle);
    micBtn      = make ("MICRO", K::Toggle);
    vangBtn     = make ("VANG", K::Toggle);
    danhSachBtn = make (juce::String::fromUTF8 ("DANH S\xC3\x81" "CH"), K::Action);   // list button (UTF-8 escaped: the app font draws it)
    recBtn      = make ("RECORD", K::Record);                                // own red look, far right, with its clock inside
    toneDownBtn = make (ha, K::Step);
    toneUpBtn   = make (nang, K::Step);
    keyDownBtn  = make (juce::String::fromUTF8 ("\xE2\x88\x92"), K::Step);   // minus sign
    keyUpBtn    = make ("+", K::Step);
    compactBtn  = make ({}, K::Chevron);

    // Bigger text on the main controls (easier to read on stage).
    for (auto* b : { toneDownBtn.get(), toneUpBtn.get() })
        b->setFontSize (kCtlFont);
    for (auto* b : { beatBtn.get(), micBtn.get(), vangBtn.get(), recBtn.get() })
    {
        b->setFontSize (kTrackFont);
        b->setTight (true);
    }
    danhSachBtn->setFontSize (14.5f);
    danhSachBtn->setProminent (true);                    // stands out in the title row: tinted body + list icon
    for (auto* b : { keyDownBtn.get(), keyUpBtn.get() })
        b->setFontSize (kSignFont);

    // BEAT / MICRO / VANG: the border blinks red (500 ms) while the track / FX is OFF.
    for (auto* b : { beatBtn.get(), micBtn.get(), vangBtn.get() })
        b->setBlinkWhenOff (true);

    // ONE "+" (right of KEY): its menu adds a SLIDER (track volume / pan, any VST parameter, send level) or a TOGGLE.
    // There are NO built-in sliders any more: the slider rows only show what the user added.
    addBtn = make ("+", K::Action);
    addBtn->setFontSize (16.0f);
    addBtn->setTooltip (juce::String::fromUTF8 ("Thêm slider (volume / pan track, parameter VST, mức send) hoặc toggle (bật / tắt)"));
    addBtn->onClick = [this] { showAddMenu(); };

    beatBtn->onClick = [this] { live.setBeatOn (beatBtn->getToggleState()); };
    micBtn->onClick  = [this] { live.setMicOn  (micBtn->getToggleState()); };
    vangBtn->onClick = [this] { live.setVangOn (vangBtn->getToggleState()); };

    toneDownBtn->onClick = [this] { live.stepTone (-1); };
    toneUpBtn->onClick   = [this] { live.stepTone (+1); };
    keyDownBtn->onClick  = [this] { live.stepKeyOffset (-1); };
    keyUpBtn->onClick    = [this] { live.stepKeyOffset (+1); };
    compactBtn->onClick  = [this] { if (onToggleCompact) onToggleCompact(); };
    danhSachBtn->onClick = [this] { if (onShowHistory) onShowHistory(); };
    recBtn->onClick       = [this] { if (onRecordPressed) onRecordPressed(); };

    danhSachBtn->setTooltip (juce::String::fromUTF8 ("Danh s\xC3\xA1" "ch c\xC3\xA1" "c b\xC3\xA0" "i h\xC3\xA1" "t \xC4\x91\xC3\xA3 t\xE1\xBB\xAB" "ng m\xE1\xBB\x9F"));
    recBtn->setTooltip      (juce::String::fromUTF8 ("Ghi \xC3\xA2" "m MASTER ra file WAV"));
    // Record: red ring by default, solid red pulse (500 ms) while recording (see refresh()).

    beatBtn->setTooltip ("BEAT on / off");
    micBtn->setTooltip  ("MICRO on / off");
    vangBtn->setTooltip (juce::String::fromUTF8 ("B\xE1\xBA\xADt / t\xE1\xBA\xAFt c\xC3\xA1" "c FX \xC4\x91" "ang g\xE1\xBB\xAD" "i t\xE1\xBB\x9B" "i c\xE1\xBB\x99" "t MICRO"));
    toneDownBtn->setTooltip (juce::String::fromUTF8 ("H\xE1\xBA\xA1 tone nh\xE1\xBA\xA1" "c (1 b\xC3\xA1" "n cung)"));
    toneUpBtn->setTooltip   (juce::String::fromUTF8 ("N\xC3\xA2ng tone nh\xE1\xBA\xA1" "c (1 b\xC3\xA1" "n cung)"));
    keyDownBtn->setTooltip  (juce::String::fromUTF8 ("Ch\xE1\xBB\x89 h\xE1\xBA\xA1 KEY g\xE1\xBB\xAD" "i Autotune, kh\xC3\xB4" "ng \xC4\x91\xE1\xBB\x95" "i nh\xE1\xBA\xA1" "c"));
    keyUpBtn->setTooltip    (juce::String::fromUTF8 ("Ch\xE1\xBB\x89 n\xC3\xA2ng KEY g\xE1\xBB\xAD" "i Autotune, kh\xC3\xB4" "ng \xC4\x91\xE1\xBB\x95" "i nh\xE1\xBA\xA1" "c"));

    masterMeter = std::make_unique<MasterMeter> (live);
    addChildComponent (*masterMeter);                // hidden until the panel is collapsed

    setOpaque (true);
    live.onChanged = [this] { refresh(); };
    refresh();
    startTimerHz (30);          // 30 Hz: smooth marquee; the controller state is polled every 4th tick (~7 Hz)
}

ControlBar::~ControlBar()
{
    stopTimer();
    live.onChanged = nullptr;
}

juce::String ControlBar::formatSteps (int steps)
{
    if (steps == 0) return "0";
    const double cung = std::abs (steps) * 0.5;                       // 1 step = 1 semitone = 0.5 cung
    const auto s = (steps % 2 == 0) ? juce::String (juce::roundToInt (cung)) : juce::String (cung, 1);
    return (steps > 0 ? "+" : "-") + s;
}

void ControlBar::setStatus (const juce::String& text, StatusKind kind)
{
    if (text == statusText && kind == statusKind && ! retryAvailable) return;
    statusText = text;
    statusKind = kind;
    detailText = {};
    retryAvailable = false;
    overRetry = false;
    repaint (readout);
}

void ControlBar::setFailure (const juce::String& shortText, const juce::String& detail)
{
    statusText = shortText;
    statusKind = StatusKind::Warn;
    detailText = detail;
    retryAvailable = true;
    overRetry = false;
    repaint (readout);
}

// The title is shown from its first LETTER: counters like "(2)", time stamps like "[00:00 / 06:02]",
// digits, brackets and other symbols in front of it are cut. Nothing is cut at the end.
static juce::String stripLeadingNonLetters (const juce::String& raw)
{
    const auto trimmed = raw.trim();
    auto p = trimmed.getCharPointer();

    while (! p.isEmpty())
    {
        const juce_wchar c = *p;
        const bool letter = juce::CharacterFunctions::isLetter (c)
                         || (c >= 0x00C0 && c <= 0x024F)          // Latin accented (Vietnamese)
                         || (c >= 0x1E00 && c <= 0x1EFF);         // Latin Extended Additional (Vietnamese)
        if (letter) break;
        ++p;
    }

    const auto cut = juce::String (p).trim();
    return cut.isNotEmpty() ? cut : trimmed;                      // no letter at all: keep what we got
}

void ControlBar::setSongTitle (const juce::String& title, bool skipped)
{
    // The tag goes on AFTER the cut of leading symbols, otherwise its "[" would be cut as well.
    const auto cleaned = stripLeadingNonLetters (title);
    const auto t = skipped && cleaned.isNotEmpty() ? juce::String::fromUTF8 ("[B\xE1\xBB\x8F qua] ") + cleaned : cleaned;
    if (t == songTitle) return;
    const auto now = juce::Time::getMillisecondCounter();
    prevTitle   = songTitle;                                 // old title flies away first (if there was one)
    prevSkipped = songSkipped;
    songSkipped = skipped && cleaned.isNotEmpty();
    outStartMs  = now;
    songTitle   = t;
    marqueeStartMs = prevTitle.isNotEmpty() ? now + (juce::uint32) outMs : now;   // new title flies in AFTER the old one left
    repaint (titleStrip);
}

void ControlBar::flashResult()
{
    flashStartMs = juce::Time::getMillisecondCounter();
    flashing = true;
    repaint (readout);
}

void ControlBar::setLoading (bool on)
{
    if (on == loading) return;
    loading = on;
    if (on)
    {
        flashing = false;                  // new search: no blink, no old error text
        statusText = {};
        statusKind = StatusKind::Idle;
        detailText = {};
        retryAvailable = false;
        overRetry = false;
    }
    repaint();
}

int ControlBar::meterReserve() const
{
    return 0;                                               // no master meter any more: the title bar's live wave shows the level
}

void ControlBar::setCompact (bool compact)
{
    compactMode = compact;
    // (the master meter stays hidden: the title bar's wave replaced it)
    resized();
    compactBtn->setPointsUp (! compact);     // up = "collapse", down = "expand"
    compactBtn->setTooltip (compact ? juce::String::fromUTF8 ("M\xE1\xBB\x9F r\xE1\xBB\x99ng pt.Live")
                                    : juce::String::fromUTF8 ("Thu g\xE1\xBB\x8Dn ch\xE1\xBB\x89 c\xC3\xB2n b\xE1\xBA\xA3ng \xC4\x91i\xE1\xBB\x81u khi\xE1\xBB\x83n"));
}

// The first toggles sit in the top row (right of the KEY group), the others go in the rows below the sliders.
int ControlBar::numTier1Toggles() const
{
    int n = 0;
    for (auto& u : itemUis)
        if (u.kind == LiveController::CtlItem::Kind::Toggle && n < kMaxTier1Toggles)
            ++n;
    return n;
}

bool ControlBar::inTier1 (size_t index) const
{
    if (index >= itemUis.size() || itemUis[index].kind != LiveController::CtlItem::Kind::Toggle)
        return false;

    int before = 0;
    for (size_t i = 0; i < index; ++i)
        if (itemUis[i].kind == LiveController::CtlItem::Kind::Toggle)
            ++before;
    return before < kMaxTier1Toggles;
}

std::vector<std::pair<ControlBar::ParamSlider*, int>> ControlBar::gridSliders() const
{
    std::vector<std::pair<ParamSlider*, int>> v;
    for (size_t i = 0; i < itemUis.size(); ++i)
        if (itemUis[i].slider != nullptr)
            v.emplace_back (itemUis[i].slider.get(), (int) i);
    return v;
}

// Layout below tier 1. ALL the sliders (they are all user-made) form ONE grid:
//   up to 5 sliders = one row (they shrink to fit), 6 = 3+3, 7 = 4+3, 8 = 4+4, ...
// If the width is too small for that, rows are added (never narrower than kMinTrackW per line).
// The toggles that did not fit in tier 1 are packed left to right in rows under the grid.
// Rects are relative to the top of tier 2; empty rect = the item is in the top row.
ControlBar::ExtraLayout ControlBar::computeExtras (int width) const
{
    constexpr int pad = 14, sliderH = 24;

    ExtraLayout out;
    out.rects.assign (itemUis.size(), juce::Rectangle<int>());

    const int avail = juce::jmax (1, width - 2 * pad);

    //== slider grid ====================================================================
    const auto sliders = gridSliders();
    const int n = (int) sliders.size();

    std::vector<int> fixed;
    for (auto& s : sliders) fixed.push_back (s.first->getFixedWidth());

    auto fits = [&] (int rowsN)
    {
        const auto counts = splitSliderRows (n, rowsN);
        int from = 0;
        for (int cnt : counts)
        {
            if (sliderRowMinWidth (fixed, from, cnt, n) > avail) return false;
            from += cnt;
        }
        return true;
    };

    int rowsN = desiredSliderRows (n);
    while (rowsN > 0 && rowsN < n && ! fits (rowsN))
        ++rowsN;

    const auto counts = splitSliderRows (n, rowsN);
    int from = 0;
    for (size_t ri = 0; ri < counts.size(); ++ri)
    {
        const int cnt = counts[ri];

        int gaps = 0, fixedSum = 0;
        for (int j = 0; j < cnt; ++j)
        {
            fixedSum += fixed[(size_t) (from + j)];
            if (j > 0) gaps += sliderGap (n, j);
        }

        const int totalLine = avail - gaps - fixedSum;
        const int base = juce::jmax (kMinTrackW, totalLine / cnt);
        const int rem  = totalLine >= cnt * kMinTrackW ? totalLine % cnt : 0;

        int x = pad;
        for (int j = 0; j < cnt; ++j)
        {
            if (j > 0) x += sliderGap (n, j);
            const int w = fixed[(size_t) (from + j)] + base + (j < rem ? 1 : 0);
            const juce::Rectangle<int> r { x, (int) ri * kExtraRowH + 3, w, sliderH };

            const int k = from + j;
            out.rects[(size_t) sliders[(size_t) k].second] = r;
            x += w;
        }
        from += cnt;
    }
    out.gridRows = (int) counts.size();

    //== toggles that are not in the top row ============================================
    struct Cell { size_t index; };
    std::vector<std::vector<Cell>> rows;
    int used = 0;

    for (size_t i = 0; i < itemUis.size(); ++i)
    {
        if (inTier1 (i) || itemUis[i].slider != nullptr) continue;

        if (rows.empty() || (! rows.back().empty() && used + kExtraGap + kExtraToggleW > avail))
        {
            rows.emplace_back();
            used = 0;
        }
        used += (rows.back().empty() ? 0 : kExtraGap) + kExtraToggleW;
        rows.back().push_back ({ i });
    }

    for (size_t ri = 0; ri < rows.size(); ++ri)
    {
        int x = pad;
        for (auto& c : rows[ri])
        {
            out.rects[c.index] = { x, (out.gridRows + (int) ri) * kExtraRowH + 3, kExtraToggleW, sliderH };
            x += kExtraToggleW + kExtraGap;
        }
    }

    out.rows = out.gridRows + (int) rows.size();
    return out;
}

int ControlBar::getIdealHeightForWidth (int width) const
{
    return theme::controlBarH - kExtraRowH + computeExtras (width - meterReserve()).rows * kExtraRowH;     // no sliders / toggles = no row
}

// Narrowest width that still shows everything. KEEP IN SYNC with resized() (same constants).
int ControlBar::getIdealWidth() const
{
    constexpr int pad = 14, gap = 6;
    constexpr int trackW = 3 * kTrackBtnW + 2 * gap, keyW = 2 * 30 + 44 + 2 * gap;
    const int toneW = 2 * toneButtonWidth() + 44 + 2 * gap;

    // Tier 1: pad + [BEAT MICRO VANG] + KEY/SCALE box (min 105) + TONE + KEY + [TOGGLE ...] ... [RECORD] + pad,
    // with the cluster gap (12), the three group gaps (3 x 14) and the clear space before RECORD (kRecGap).
    // This also clears the title bar's needs.
    const int tier1 = 2 * pad + trackW + 12 + 105 + 3 * 14 + toneW + keyW
                    + toggleGroupWidth (numTier1Toggles()) + kRecGap + kRecBtnW;

    // Sliders: the widest row of the slider grid at its minimal line length.
    const auto sliders = gridSliders();
    const int n = (int) sliders.size();
    std::vector<int> fixed;
    for (auto& s : sliders) fixed.push_back (s.first->getFixedWidth());

    int tier2 = 0, from = 0;
    for (int cnt : splitSliderRows (n, desiredSliderRows (n)))
    {
        tier2 = juce::jmax (tier2, 2 * pad + sliderRowMinWidth (fixed, from, cnt, n));
        from += cnt;
    }

    return juce::jmax (tier1, tier2) + meterReserve();
}

void ControlBar::refresh()
{
    beatBtn->setToggleState (live.isBeatOn(), juce::dontSendNotification);
    micBtn->setToggleState  (live.isMicOn(),  juce::dontSendNotification);
    vangBtn->setToggleState (live.isVangOn(), juce::dontSendNotification);
    vangBtn->setEnabled (live.hasVang());
    const bool recNow = isRecording != nullptr && isRecording();
    recBtn->setRecording (recNow);
    {
        const int total = recNow && getRecordSeconds ? juce::jmax (0, (int) getRecordSeconds()) : 0;
        const auto t = juce::String (total / 3600) + ":" + juce::String ((total / 60) % 60).paddedLeft ('0', 2)
                     + ":" + juce::String (total % 60).paddedLeft ('0', 2);
        if (t != recTimeText || recNow != recClockLive)          // the clock lives above the RECORD button (its caption slot)
        {
            recTimeText  = t;
            recClockLive = recNow;
            repaint (recGroup);
        }
    }

    syncItems();                    // user-made sliders / toggles: create / remove the components if the list changed
    updateItems();                  // and follow the live state of each one

    // While a song is analysed the tone / key buttons are locked (same as ptStudio).
    const bool free = ! live.isBusy();
    for (auto* b : { toneDownBtn.get(), toneUpBtn.get(), keyDownBtn.get(), keyUpBtn.get() })
        b->setEnabled (free);

    const auto key = live.hasKey() ? live.getFinalRootName() + " " + live.getScaleName().toUpperCase()
                                   : juce::String();
    const auto signature = key + "|" + formatSteps (live.getToneSteps()) + "|" + formatSteps (live.getKeyOffset());
    if (signature != shownKey)
    {
        shownKey = signature;
        repaint();
    }
}

//==============================================================================
// User-made sliders / toggles
//==============================================================================
void ControlBar::syncItems()
{
    using Kind = LiveController::CtlItem::Kind;

    const auto items = live.getItems();

    juce::String signature;
    for (auto& it : items)
        signature << it.id << (it.kind == Kind::Toggle ? 't' : 's') << (int) it.kind << ',';

    if (signature == itemsSignature)
        return;
    itemsSignature = signature;

    closeRenameEditor();                      // the list changed under it: drop the editor (nothing is applied)

    std::vector<ItemUi> next;
    for (auto& it : items)
    {
        ItemUi ui;
        bool reused = false;

        for (auto& old : itemUis)
            if (old.id == it.id && old.kind == it.kind && (old.button != nullptr || old.slider != nullptr))
            {
                ui = std::move (old);
                reused = true;
                break;
            }

        if (! reused)
        {
            const int id = it.id;
            ui.id = id;
            ui.kind = it.kind;

            if (it.kind == Kind::Toggle)
            {
                auto b = std::make_unique<PanelButton> (juce::String(), PanelButton::Kind::Toggle);
                auto* raw = b.get();
                addAndMakeVisible (*b);
                b->setFontSize (theme::szBody);                 // readable on stage
                b->setTight (true);                             // text is squeezed instead of cut
                b->setDoubleClickSwallowsClick (true);
                b->onClick      = [this, id, raw]
                {
                    if (live.isUnassigned (id))                   // empty toggle: a click opens "assign a function"
                    {
                        raw->setToggleState (false, juce::dontSendNotification);
                        showAddMenu (id);
                        return;
                    }
                    live.setToggleOn (id, raw->getToggleState());
                };
                b->onRightClick = [this, id] { showItemMenu (id); };
                b->onDoubleClick = [this, id]
                {
                    // The 1st click of the double-click already switched it: put it back, then rename.
                    const auto v = live.getToggleView (id);
                    if (v.available && ! v.locked)
                        live.setToggleOn (id, ! v.on);
                    beginRename (id);
                };
                ui.button = std::move (b);
            }
            else
            {
                auto sl = std::make_unique<ParamSlider> (juce::String());
                addAndMakeVisible (*sl);
                sl->setUserMade (kExtraLabelMax);
                sl->setValueWidth (kExtraValueW);
                sl->wheelStep = 0.01;
                sl->defaultValue = live.getSliderDefaultPos (id);
                sl->formatValue  = [this, id] (double p) { return live.getSliderText (id, p); };
                sl->onUserChange = [this, id] (double p) { live.setSliderPos (id, p); };      // drag = the VST moves right now
                sl->onRightClick = [this, id] { showItemMenu (id); };
                sl->onClickUnavailable = [this, id] { if (live.isUnassigned (id)) showAddMenu (id); else showItemMenu (id); };
                sl->onLabelDoubleClick = [this, id] { beginRename (id); };
                ui.slider = std::move (sl);
            }
        }

        next.push_back (std::move (ui));
    }

    itemUis = std::move (next);               // components of removed items are deleted here

    resized();
    repaint();
    if (onIdealWidthChanged) onIdealWidthChanged();      // the window follows (wider minimum, taller panel)
}

void ControlBar::updateItems()
{
    bool relayout = false;

    for (auto& u : itemUis)
    {
        if (u.button != nullptr)
        {
            const auto v = live.getToggleView (u.id);

            if (renameEditor == nullptr || renameId != u.id)
                u.button->setButtonText (v.label);

            u.button->setHollow (! v.available);                          // nothing assigned / target gone: empty dot, dim text
            u.button->setEnabled (! (v.available && v.locked));
            u.button->setToggleState (v.available && v.on, juce::dontSendNotification);

            if (v.tip != u.tip) { u.tip = v.tip; u.button->setTooltip (v.tip); }
        }
        else if (u.slider != nullptr)
        {
            const auto v = live.getSliderView (u.id);

            if (u.slider->getLabel() != v.label)
            {
                u.slider->setLabel (v.label);                              // the name column got wider / narrower
                relayout = true;
            }

            u.slider->setAvailable (v.available);
            if (v.available) u.slider->setValue (v.pos);

            if (v.tip != u.tip) { u.tip = v.tip; u.slider->setTooltip (v.tip); }
        }
    }

    if (relayout)
    {
        resized();
        repaint();
        if (onIdealWidthChanged) onIdealWidthChanged();
    }
}

void ControlBar::timerCallback()
{
    if (! isShowing())
        return;                            // window minimised: no polling, no animation

    if (++tick % 4 == 0)
        refresh();                         // state poll ~7 Hz

    if (loading || (songTitle.isNotEmpty() && (titleW > titleText.getWidth()
                                               || (int) (juce::Time::getMillisecondCounter() - marqueeStartMs) < 3600
                                               || (prevTitle.isNotEmpty() && juce::Time::getMillisecondCounter() - outStartMs < (juce::uint32) outMs + 100))))
        repaint (titleStrip);              // falling letters / scrolling long title / loading bar

    if (flashing && juce::Time::getMillisecondCounter() - flashStartMs >= 2000)
    {
        flashing = false;
        repaint (readout);
    }

    if (loading || flashing)
        repaint (readout);                 // analysing animation + pulsing border / key blink
}

//==============================================================================
void ControlBar::resized()
{
    constexpr int pad = 14, gap = 6;
    constexpr int titleH = 30;                        // tier 0: song title (marquee) + DANH SÁCH (far right)
    constexpr int tier1Y = titleH, tier1H = 60;       // tier 1: buttons + key readout

    constexpr int btnH = 32;
    const int btnY = tier1Y + 22;

    // Tier 1, left -> right:  [BEAT][MICRO][VANG]  [ KEY / SCALE ]  |  TONE  |  KEY  |  [TOGGLE ...]  .....  |  [RECORD 0:00:00]
    // RECORD lives alone at the far right: far from the performance controls, with its own divider and clock.
    const int toneBtnW = toneButtonWidth();           // HA and NANG: same width
    constexpr int trackBtnW = kTrackBtnW, toneValW = 44, keyBtnW = 30, keyValW = 44;
    constexpr int readoutMinW = 105, readoutMaxW = 175, recBtnW = kRecBtnW, clusterGap = 12;
    constexpr int trackW = 3 * trackBtnW + 2 * gap;
    const int toneW  = 2 * toneBtnW + toneValW + 2 * gap;
    constexpr int keyW   = 2 * keyBtnW + keyValW + 2 * gap;
    constexpr int toggleW = kToggleW, toggleGap = kToggleGap;

    // The first toggles (max 4) live in tier 1 ([+] and [^] are in the title row); more toggles and all user-made sliders go in rows below.
    const int numShown = numTier1Toggles();
    const int toggleGroupW = toggleGroupWidth (numShown);

    const int fixedW = 2 * pad + trackW + clusterGap + toneW + keyW + 12 + kRecGap + recBtnW;

    // The key / scale box takes the spare width (up to readoutMaxW); what is left spreads the 3 groups a little.
    const int usableW  = getWidth() - meterReserve();          // the master meter (collapsed panel) takes the right edge
    const int spare    = usableW - fixedW - toggleGroupW;
    const int readoutW = juce::jlimit (readoutMinW, readoutMaxW, spare - 3 * 22);
    const int groupGap = juce::jlimit (14, 28, (spare - readoutW) / 3);

    // Tier 0: title strip over the whole width, the marquee on the left and, at the far right:  [DANH SÁCH] [+] [^]
    constexpr int listBtnW = 112;
    titleStrip = { 0, 0, getWidth(), titleH };
    if (masterMeter != nullptr)
        masterMeter->setBounds (getWidth() - kMeterW, 1, kMeterW, getHeight() - 1);   // flush right + bottom; starts UNDER the 1px blue top line so that line runs unbroken to the edge
    const int titleBtnY = (titleH - kTitleBtnH) / 2;
    compactBtn->setBounds (usableW - pad - kTitleBtnW, titleBtnY, kTitleBtnW, kTitleBtnH);
    addBtn->setBounds (compactBtn->getX() - kTitleBtnGap - kTitleBtnW, titleBtnY, kTitleBtnW, kTitleBtnH);
    danhSachBtn->setBounds (addBtn->getX() - kTitleBtnGap - listBtnW, titleBtnY, listBtnW, kTitleBtnH);
    titleText  = { pad, 0, juce::jmax (0, danhSachBtn->getX() - 2 * gap - pad), titleH };

    int x = pad;
    auto place = [&] (PanelButton& b, int w) { b.setBounds (x, btnY, w, btnH); x += w; };

    // TRACK: [BEAT][MICRO][VANG]
    const int trackStart = x;
    place (*beatBtn, trackBtnW);  x += gap;
    place (*micBtn,  trackBtnW);  x += gap;
    place (*vangBtn, trackBtnW);
    trackGroup = { trackStart, tier1Y, x - trackStart, tier1H };
    x += clusterGap;

    // KEY / SCALE box: between VANG and TONE
    readout = { x, tier1Y + 4, readoutW, tier1H - 8 };
    x += readoutW;
    tier1DividerX[0] = x + groupGap / 2;
    x += groupGap;

    // Record: far right, own group (right-aligned on the usable width), divider on its left.
    {
        const int recX = usableW - pad - recBtnW;
        recBtn->setBounds (recX, btnY, recBtnW, btnH);
        recGroup = { recX, tier1Y, recBtnW, tier1H };
        tier1DividerX[3] = recX - kRecGap / 2;
    }

    // TONE (HẠ / value / NÂNG) - compact
    const int toneStart = x;
    place (*toneDownBtn, toneBtnW); x += gap;
    toneValue = { x, btnY, toneValW, btnH }; x += toneValW + gap;
    place (*toneUpBtn, toneBtnW);
    toneGroup = { toneStart, tier1Y, x - toneStart, tier1H };
    tier1DividerX[1] = x + groupGap / 2;
    x += groupGap;

    // KEY (− / value / +) - compact
    const int keyStart = x;
    place (*keyDownBtn, keyBtnW); x += gap;
    keyValue = { x, btnY, keyValW, btnH }; x += keyValW + gap;
    place (*keyUpBtn, keyBtnW);
    keyGroup = { keyStart, tier1Y, x - keyStart, tier1H };
    tier1DividerX[2] = numShown > 0 ? x + groupGap / 2 : -1;      // the 3rd divider only when toggles follow
    if (numShown > 0)
        x += groupGap;

    // Toggle buttons of the top row (same height as the other tier-1 buttons)
    for (size_t i = 0; i < itemUis.size(); ++i)
        if (inTier1 (i) && itemUis[i].button != nullptr)
        {
            itemUis[i].button->setBounds (x, btnY, toggleW, btnH);
            x += toggleW + toggleGap;
        }

    // Tier 2 + rows below: ALL sliders (user-made) form one grid (1-5 = one row, 6 = 3+3, 7 = 4+3, 8 = 4+4 ...),
    // then the toggles that did not fit in tier 1. Tier 2 starts at controlBarH - kExtraRowH; the panel is one row (30 px)
    // taller for every row (no sliders and no extra toggles = no row at all).
    const auto extras = computeExtras (usableW);
    constexpr int tier2Top = theme::controlBarH - kExtraRowH;
    extraRows    = extras.rows;
    gridRows     = extras.gridRows;
    toggleRows   = extras.rows - extras.gridRows;


    for (size_t i = 0; i < itemUis.size(); ++i)
    {
        if (inTier1 (i)) continue;
        const auto r = extras.rects[i].translated (0, tier2Top);
        if (itemUis[i].button != nullptr)      itemUis[i].button->setBounds (r);
        else if (itemUis[i].slider != nullptr) itemUis[i].slider->setBounds (r);
    }

    if (renameEditor != nullptr && renameId >= 0)
        for (auto& u : itemUis)
            if (u.id == renameId)
            {
                auto r = u.button != nullptr ? u.button->getBounds() : u.slider->getBounds().withWidth (juce::jmin (u.slider->getWidth(), 170));
                renameEditor->setBounds (r);
            }

    // The window needs to know when the number of rows changes (the panel is taller / shorter).
    const int h = tier2Top + extraRows * kExtraRowH;
    if (h != lastReportedHeight)
    {
        lastReportedHeight = h;
        juce::Component::SafePointer<ControlBar> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->onIdealWidthChanged) safe->onIdealWidthChanged(); });
    }
}

juce::Rectangle<int> ControlBar::getHistoryButtonScreenBounds() const
{
    return danhSachBtn != nullptr ? danhSachBtn->getScreenBounds() : getScreenBounds();
}

juce::Rectangle<int> ControlBar::readoutBounds() const { return readout; }

juce::Rectangle<int> ControlBar::retryPillBounds() const
{
    if (! retryAvailable || loading || readout.getWidth() < 60) return {};
    const int w = juce::jmin (92, readout.getWidth() - 24), h = 17;
    return { readout.getCentreX() - w / 2, readout.getBottom() - h - 7, w, h };
}

void ControlBar::mouseMove (const juce::MouseEvent& e)
{
    const bool over  = readout.contains (e.getPosition()) && ! live.isBusy();
    const bool pill  = retryPillBounds().contains (e.getPosition());
    if (over != overReadout || pill != overRetry)
    {
        overReadout = over;
        overRetry   = pill;
        setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint (readout);
    }
}

void ControlBar::mouseExit (const juce::MouseEvent&)
{
    if (overReadout || overRetry)
    {
        overReadout = overRetry = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint (readout);
    }
}

void ControlBar::mouseDown (const juce::MouseEvent& e)
{
    if (retryPillBounds().contains (e.getPosition()))
    {
        if (onRetry) onRetry();                       // THỬ LẠI
        return;
    }
    if (readout.contains (e.getPosition()) && ! live.isBusy())
        showKeyMenu();
}

// The single "+": SLIDER or TOGGLE, then everything is chosen in nested sub-menus.
//   Slider > <track> > <VST>            -> a searchable list of that VST's parameters opens
//   Slider > <track> > Send > <FX>      -> the send level of that track to that FX
//   Toggle > <function> > <track> [> <VST / send>]     (one function = one button)
void ControlBar::showAddMenu (int assignId)
{
    using Fn = LiveController::CtlItem::Fn;
    using CtlItem = LiveController::CtlItem;

    const auto tracksInfo = live.getControlTracks();

    // Every menu entry gets an id = index (+1) into this list of actions.
    auto actions = std::make_shared<std::vector<std::function<void()>>>();
    auto act = [actions] (std::function<void()> f)
    {
        actions->push_back (std::move (f));
        return (int) actions->size();
    };

    juce::Component::SafePointer<ControlBar> safe (this);

    // assignId >= 0: the menu fills an EMPTY item (placeholder) in place; otherwise it adds a new one.
    bool assignToggle = false;
    juce::Component* anchor = addBtn.get();
    if (assignId >= 0)
    {
        for (auto& it : live.getItems())
            if (it.id == assignId) assignToggle = it.kind == CtlItem::Kind::Toggle;
        for (auto& u : itemUis)
            if (u.id == assignId)
                anchor = u.button != nullptr ? static_cast<juce::Component*> (u.button.get())
                                             : static_cast<juce::Component*> (u.slider.get());
    }

    auto apply = [safe, assignId] (CtlItem item)
    {
        if (safe == nullptr) return;
        if (assignId >= 0) safe->live.assignItem (assignId, item);
        else               safe->live.addItem (item);
        safe->refresh();
    };

    //== SLIDER ==============================================================================
    juce::PopupMenu sliderMenu;
    for (auto& t : tracksInfo)
    {
        const auto inserts = live.getTrackInserts (t.id);
        const auto sends   = live.getTrackSends (t.id);

        juce::PopupMenu m;

        // Volume / pan of the track itself (one slider each per track).
        using Kd = LiveController::CtlItem::Kind;
        m.addSectionHeader (juce::String::fromUTF8 ("Track này"));
        for (auto kd : { Kd::VolumeSlider, Kd::PanSlider })
        {
            const bool exists = live.hasTrackSlider (kd, t.id);
            const auto label = juce::String (kd == Kd::VolumeSlider ? "Volume" : "Pan");
            m.addItem (act ([apply, tid = t.id, kd]
                            { CtlItem it; it.kind = kd; it.trackId = tid; apply (it); }),
                       exists ? label + juce::String::fromUTF8 ("   (đã có slider)") : label,
                       ! exists, exists);
        }

        m.addSeparator();
        m.addSectionHeader (juce::String::fromUTF8 ("VST trên track này"));
        if (inserts.isEmpty())
            m.addItem (-1, juce::String::fromUTF8 ("(chưa có VST nào trong INSERT)"), false);

        for (auto& ins : inserts)
            m.addItem (act ([safe, assignId, tid = t.id, pid = ins.id, pname = ins.name]
                            { if (safe != nullptr) safe->pickParameter (tid, pid, pname, assignId); }),
                       ins.name);

        if (! sends.isEmpty())
        {
            m.addSeparator();
            m.addSectionHeader (juce::String::fromUTF8 ("Mức send"));
            for (auto& sd : sends)
                m.addItem (act ([apply, tid = t.id, bus = sd.bus]
                                { CtlItem it; it.kind = CtlItem::Kind::SendSlider; it.trackId = tid; it.bus = bus; apply (it); }),
                           juce::String::fromUTF8 ("Send > ") + sd.name);
        }

        sliderMenu.addSubMenu (t.name, m);
    }

    //== TOGGLE ==============================================================================
    // One function = one button: a function that already has its button is shown ticked and cannot be added twice.
    auto addToggleItem = [&] (juce::PopupMenu& menu, const juce::String& label, Fn fn,
                              const juce::String& tid, const juce::String& pid, int bus, bool usable = true)
    {
        const bool exists = live.hasToggle (fn, tid, pid, bus);
        menu.addItem (act ([apply, fn, tid, pid, bus]
                           {
                               CtlItem it;
                               it.kind = CtlItem::Kind::Toggle;
                               it.fn = fn; it.trackId = tid; it.pluginId = pid; it.bus = bus;
                               apply (it);
                           }),
                      exists ? label + juce::String::fromUTF8 ("   (đã có nút)") : label,
                      usable && ! exists, exists);
    };

    juce::PopupMenu muteMenu, soloMenu, vstMenu, insMenu, sendsMenu, sendMenu;
    for (auto& t : tracksInfo)
    {
        const auto inserts = live.getTrackInserts (t.id);
        const auto sends   = live.getTrackSends (t.id);

        if (! t.isMaster)
        {
            addToggleItem (muteMenu, t.name, Fn::TrackOn,   t.id, {}, -1);
            addToggleItem (soloMenu, t.name, Fn::TrackSolo, t.id, {}, -1);
        }

        juce::PopupMenu vm;
        for (auto& ins : inserts)
            addToggleItem (vm, ins.name, Fn::PluginOn, t.id, ins.id, -1);
        vstMenu.addSubMenu (t.name, vm, ! inserts.isEmpty());

        addToggleItem (insMenu, t.name, Fn::InsertsOn, t.id, {}, -1, ! inserts.isEmpty());

        if (! t.isMaster)
        {
            addToggleItem (sendsMenu, t.name, Fn::SendsOn, t.id, {}, -1, ! sends.isEmpty());

            juce::PopupMenu sm;
            for (auto& sd : sends)
                addToggleItem (sm, sd.name, Fn::SendOn, t.id, {}, sd.bus);
            sendMenu.addSubMenu (t.name, sm, ! sends.isEmpty());
        }
    }

    juce::PopupMenu toggleMenu;
    addToggleItem (toggleMenu, juce::String::fromUTF8 ("ALL EFFECT (tắt / bật toàn bộ INSERT + SEND, mọi track kể cả Master)"),
                   Fn::AllEffects, {}, {}, -1);
    toggleMenu.addSeparator();
    toggleMenu.addSubMenu (juce::String::fromUTF8 ("Track bật / tắt tiếng (như BEAT / MICRO)"), muteMenu);
    toggleMenu.addSubMenu (juce::String::fromUTF8 ("Solo track"),                          soloMenu);
    toggleMenu.addSubMenu (juce::String::fromUTF8 ("VST bật / tắt (một VST)"),             vstMenu);
    toggleMenu.addSubMenu (juce::String::fromUTF8 ("Tất cả INSERT của track bật / tắt"),   insMenu);
    toggleMenu.addSubMenu (juce::String::fromUTF8 ("Tất cả SEND của track bật / tắt"),     sendsMenu);
    toggleMenu.addSubMenu (juce::String::fromUTF8 ("Một SEND (tới FX / Master) bật / tắt"), sendMenu);
    if (live.isPdcOn != nullptr)
    {
        toggleMenu.addSeparator();
        addToggleItem (toggleMenu, juce::String::fromUTF8 ("PDC (bù trễ plugin)"), Fn::Pdc, {}, {}, -1);
    }

    juce::PopupMenu menu;
    if (assignId < 0)
    {
        menu.addSectionHeader (juce::String::fromUTF8 ("Thêm vào thanh điều khiển"));
        menu.addSubMenu (juce::String::fromUTF8 ("Slider (volume / pan track, parameter VST, mức send)"), sliderMenu);
        menu.addSubMenu (juce::String::fromUTF8 ("Toggle (bật / tắt)"),                   toggleMenu);
    }
    else if (assignToggle)                    // a toggle placeholder can only become a toggle ...
    {
        menu.addSectionHeader (juce::String::fromUTF8 ("Gán chức năng cho toggle"));
        menu.addSubMenu (juce::String::fromUTF8 ("Chọn chức năng bật / tắt"), toggleMenu);
    }
    else                                      // ... and a slider placeholder only a slider
    {
        menu.addSectionHeader (juce::String::fromUTF8 ("Gán chức năng cho slider"));
        menu.addSubMenu (juce::String::fromUTF8 ("Chọn volume / pan track, parameter VST, mức send"), sliderMenu);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor),
                        [actions] (int id)
    {
        if (id > 0 && id <= (int) actions->size())
            (*actions)[(size_t) id - 1]();
    });
}

// Step 3 of "+" > Slider: every parameter the VST publishes, in a list with a search box.
void ControlBar::pickParameter (const juce::String& trackId, const juce::String& pluginId, const juce::String& pluginName, int assignId)
{
    auto params = live.getPluginParams (trackId, pluginId);       // loads the VST first when it was not loaded yet

    if (params.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Slider",
                                                pluginName + ": " + live.getLastParamError(), "OK");
        return;
    }

    juce::Component::SafePointer<ControlBar> safe (this);
    auto picker = std::make_unique<ParamPicker> (std::move (params), pluginName,
                                                 [safe, trackId, pluginId, assignId] (const LiveController::ParamInfo& p)
    {
        if (safe == nullptr) return;

        if (assignId >= 0)
        {
            LiveController::CtlItem it;
            it.kind = LiveController::CtlItem::Kind::ParamSlider;
            it.trackId = trackId; it.pluginId = pluginId; it.paramId = p.id; it.paramName = p.name;
            safe->live.assignItem (assignId, it);
        }
        else
            safe->live.addParamSlider (trackId, pluginId, p.id, p.name);
        safe->refresh();
    });
    picker->setSize (380, 440);

    juce::CallOutBox::launchAsynchronously (std::move (picker), addBtn->getScreenBounds(), nullptr);
}

// Right-click on a user-made slider / toggle.
void ControlBar::showItemMenu (int id)
{
    juce::Component* target = nullptr;
    for (auto& u : itemUis)
        if (u.id == id)
            target = u.button != nullptr ? static_cast<juce::Component*> (u.button.get())
                                         : static_cast<juce::Component*> (u.slider.get());
    if (target == nullptr) return;

    juce::PopupMenu menu;
    menu.addItem (3, juce::String::fromUTF8 (live.isUnassigned (id) ? "Gán chức năng..." : "Đổi chức năng..."));
    menu.addItem (1, juce::String::fromUTF8 ("Đổi tên..."));
    menu.addItem (2, juce::String::fromUTF8 ("Xoá"));

    juce::Component::SafePointer<ControlBar> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                        [safe, id] (int r)
    {
        if (safe == nullptr || r <= 0) return;

        if (r == 1)
            safe->beginRename (id);
        else if (r == 3)
            safe->showAddMenu (id);
        else
        {
            safe->closeRenameEditor();
            safe->live.removeItem (id);
            safe->refresh();
        }
    });
}

// Inline editor drawn over the button / the name of the slider. Enter / click elsewhere = keep the name,
// Esc = cancel, an empty name goes back to the default name.
void ControlBar::beginRename (int id)
{
    if (renameEditor != nullptr)
        commitRename (true);

    ItemUi* target = nullptr;
    for (auto& u : itemUis)
        if (u.id == id) target = &u;
    if (target == nullptr) return;

    renameId = id;
    renameEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *renameEditor;

    ed.setMultiLine (false);
    ed.setReturnKeyStartsNewLine (false);
    ed.setScrollbarsShown (false);
    ed.setInputRestrictions (target->button != nullptr ? 14 : 30);
    ed.setFont (theme::font (theme::szSmall, true));
    ed.setJustification (juce::Justification::centred);
    ed.setColour (juce::TextEditor::backgroundColourId,      theme::stage);
    ed.setColour (juce::TextEditor::textColourId,            theme::text);
    ed.setColour (juce::TextEditor::highlightColourId,       theme::accent.withAlpha (0.45f));
    ed.setColour (juce::TextEditor::highlightedTextColourId, theme::text);
    ed.setColour (juce::TextEditor::outlineColourId,         theme::accent);
    ed.setColour (juce::TextEditor::focusedOutlineColourId,  theme::accent);
    ed.setText (live.getItemName (id), false);

    ed.onReturnKey = [this] { commitRename (true);  };
    ed.onEscapeKey = [this] { commitRename (false); };
    ed.onFocusLost = [this] { commitRename (true);  };

    addAndMakeVisible (ed);
    resized();                                   // puts the editor over the right component
    ed.grabKeyboardFocus();
    ed.selectAll();
}

void ControlBar::closeRenameEditor()
{
    if (renameEditor == nullptr)
        return;

    // This may run inside one of the editor's own callbacks: it is removed now and deleted a moment later.
    auto* raw = renameEditor.release();
    removeChildComponent (raw);
    juce::MessageManager::callAsync ([raw] { delete raw; });
    renameId = -1;
}

void ControlBar::commitRename (bool apply)
{
    if (renameEditor == nullptr)
        return;

    const auto newName = renameEditor->getText().trim();
    const int  id      = renameId;

    closeRenameEditor();

    if (apply && id >= 0 && newName != live.getItemName (id))
        live.renameItem (id, newName);

    refresh();
}

juce::String ControlBar::getTooltip()
{
    return readout.contains (getMouseXYRelative()) ? (detailText.isNotEmpty() ? detailText : statusText) : juce::String();
}

void ControlBar::showKeyMenu()
{
    juce::PopupMenu major, minor;
    for (int i = 0; i < 12; ++i)
    {
        {
            const auto n = LiveController::displayRootName (i, false);
            major.addItem (1 + i, n + " - " + LiveController::vietnameseKeyName (n, "Major"));
        }
        {
            const auto n = LiveController::displayRootName (i, true);
            minor.addItem (101 + i, n + " - " + LiveController::vietnameseKeyName (n, "Minor"));
        }
    }

    juce::PopupMenu menu;
    menu.addSubMenu ("Major", major);
    menu.addSubMenu ("Minor", minor);

    juce::Component::SafePointer<ControlBar> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                  .withTargetScreenArea (localAreaToGlobal (readout)),
                        [safe] (int id)
    {
        if (safe == nullptr || id == 0) return;
        const bool isMinor = id >= 101;
        safe->live.setManualKey ((isMinor ? id - 101 : id - 1), isMinor);
        safe->setStatus ({}, StatusKind::Idle);          // manual pick clears any old error text
    });
}

//==============================================================================
void ControlBar::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (panel);

    // Thin accent line along the TOP edge: separates the live control panel from the DAW area above it.
    g.setColour (accent.withAlpha (0.85f));
    g.fillRect (0, 0, getWidth(), 1);

    const float twoPi = juce::MathConstants<float>::twoPi;
    const auto nowMs  = juce::Time::getMillisecondCounter();

    //== Tier 0: title of the song being sung (scrolling marquee, right -> left) =============
    {
        g.setColour (stage);
        g.fillRect (titleStrip.withTrimmedTop (1));          // keep the accent line on top visible

        // Old title: letters FLY OUT (drift up-left, fade), staggered, slow.
        if (prevTitle.isNotEmpty() && (nowMs - outStartMs) < (juce::uint32) outMs + 700)
        {
            const auto f = font (kTitleFont, true);
            g.setFont (f);
            juce::GlyphArrangement ga;
            ga.addLineOfText (f, prevTitle, 0.0f, 0.0f);
            const int n     = ga.getNumGlyphs();
            const int textW = (int) std::ceil (ga.getBoundingBox (0, -1, true).getRight());
            const int baseX = titleText.getX() + (textW <= titleText.getWidth() ? (titleText.getWidth() - textW) / 2 : 0);
            const float baseline = (float) titleText.getCentreY() - f.getHeight() * 0.5f + f.getAscent();
            const float el     = (float) (nowMs - outStartMs);
            const float stepO  = n > 1 ? juce::jmin (30.0f, 400.0f / (float) (n - 1)) : 0.0f;
            const float flyMs  = 500.0f;

            g.saveState();
            g.reduceClipRegion (titleText);
            for (int k = 0; k < n; ++k)
            {
                const auto& gl = ga.getGlyph (k);
                if (gl.isWhitespace()) continue;
                const float t = juce::jlimit (0.0f, 1.0f, (el - stepO * (float) k) / flyMs);
                if (t >= 1.0f) continue;
                const float e  = t * t * t;                          // ease-in: starts slow, accelerates away
                const float dx = -e * 90.0f;
                const float dy = -e * 16.0f;
                g.setColour ((prevSkipped ? textDim : songTitleCol).withAlpha (1.0f - e));
                g.drawSingleLineText (juce::String::charToString (gl.getCharacter()),
                                      baseX + juce::roundToInt (gl.getLeft() + dx), juce::roundToInt (baseline + dy));
            }
            g.restoreState();
        }

        if (songTitle.isNotEmpty())
        {
            // New title: letters FLY IN one after another (glide in from the right, fade in, soft ease-out),
            // slower than before. A title that does not fit then scrolls right -> left.
            const auto f = font (kTitleFont, true);
            g.setFont (f);

            juce::GlyphArrangement ga;
            ga.addLineOfText (f, songTitle, 0.0f, 0.0f);
            const int n      = ga.getNumGlyphs();
            const int textW  = (int) std::ceil (ga.getBoundingBox (0, -1, true).getRight());
            const int areaW  = titleText.getWidth();
            titleW = textW;

            const float elapsed   = (float) (int) (nowMs - marqueeStartMs);   // signed: negative while the old title is leaving
            const float stepMs    = n > 1 ? juce::jmin (75.0f, 1500.0f / (float) (n - 1)) : 0.0f;   // delay between letters
            const float flyMs     = 1100.0f;
            const float dropEndMs = stepMs * (float) juce::jmax (0, n - 1) + flyMs;
            const bool  fits      = textW <= areaW;

            // fits: centred and still.  too long: starts at the left, waits 1.5 s, then scrolls.
            int baseX = titleText.getX() + (fits ? (areaW - textW) / 2 : 0);
            int cycle = textW + 80;
            if (! fits)
            {
                const float scrollMs = elapsed - dropEndMs - 1500.0f;
                if (scrollMs > 0.0f)
                    baseX -= (int) (scrollMs * 0.05f) % cycle;                 // 50 px / s
            }

            const float baseline = (float) titleText.getCentreY() - f.getHeight() * 0.5f + f.getAscent();

            g.saveState();
            g.reduceClipRegion (titleText);

            for (int copy = 0; copy < (fits ? 1 : 2); ++copy)
            {
                const int cx = baseX + copy * cycle;
                for (int k = 0; k < n; ++k)
                {
                    const auto& gl = ga.getGlyph (k);
                    if (gl.isWhitespace()) continue;

                    const float t = juce::jlimit (0.0f, 1.0f, (elapsed - stepMs * (float) k) / flyMs);
                    if (t <= 0.0f) continue;

                    const float inv = 1.0f - t;
                    const float e   = 1.0f - inv * inv * inv;            // ease-out cubic
                    const float dx  = (1.0f - e) * 110.0f;               // glides in from the right
                    const float dy  = (1.0f - e) * 8.0f;
                    g.setColour ((songSkipped ? textDim : songTitleCol).withAlpha (juce::jmin (1.0f, e * 1.6f)));
                    g.drawSingleLineText (juce::String::charToString (gl.getCharacter()),
                                          cx + juce::roundToInt (gl.getLeft() + dx), juce::roundToInt (baseline + dy));
                }
            }

            g.restoreState();
        }
        else
        {
            g.setColour (textDim);
            g.setFont (font (szLabel));
            g.drawText (juce::String::fromUTF8 ("Chưa phát hiện bài YouTube"), titleText, juce::Justification::centred, true);
        }

        g.setColour (hairline);
        g.fillRect (0, titleStrip.getBottom() - 1, getWidth(), 1);

        // Loading: a bright segment keeps sliding left -> right along the bottom edge of the strip.
        if (loading)
        {
            const float w     = (float) getWidth();
            const float barW  = w * 0.28f;
            const float phase = (float) (nowMs % 1400) / 1400.0f;
            const float bx    = -barW + (w + barW) * phase;
            const float by    = (float) titleStrip.getBottom() - 3.0f;

            juce::ColourGradient grad (accent.withAlpha (0.0f), bx, by, accent.withAlpha (0.0f), bx + barW, by, false);
            grad.addColour (0.5, accent.withAlpha (0.95f));

            g.saveState();
            g.reduceClipRegion (0, (int) by, getWidth(), 3);
            g.setGradientFill (grad);
            g.fillRect (bx, by, barW, 3.0f);
            g.restoreState();
        }
    }

    //== Tier 1 captions / dividers ==========================================================
    g.setColour (textMid);
    g.setFont (spacedFont (kCaptionFont, 0.12f, true));
    auto caption = [&] (const juce::Rectangle<int>& grp, const juce::String& t)
    {
        g.drawText (t, grp.getX(), grp.getY() + 4, grp.getWidth(), 16, juce::Justification::centredLeft, false);
    };
    caption (trackGroup, "TRACK");
    caption (toneGroup, "TONE");
    caption (keyGroup, "KEY");

    // RECORD: the take's clock sits where its caption used to be (it replaces the "GHI ÂM" label).
    // Dim red while idle, bright red while recording.
    g.setColour (recClockLive ? dangerHover : recordRed.withAlpha (0.65f));
    g.setFont (font (kCaptionFont + 2.0f, true));
    g.drawText (recTimeText, recGroup.getX(), recGroup.getY() + 3, recGroup.getWidth(), 18, juce::Justification::centred, false);

    g.setColour (outline);
    for (int dx : tier1DividerX)
        if (dx >= 0)
            g.fillRect (dx, trackGroup.getY() + 10, 1, trackGroup.getHeight() - 20);

    // line between tier 1 and the sliders
    g.setColour (hairline);
    g.fillRect (0, trackGroup.getBottom(), getWidth(), 1);
    g.setColour (outline);

    // toggle rows under the slider grid: a quiet line above them
    if (toggleRows > 0 && gridRows > 0)
    {
        g.setColour (hairline);
        g.fillRect (14, theme::controlBarH - kExtraRowH + gridRows * kExtraRowH, getWidth() - meterReserve() - 28, 1);
    }

    // Value boxes
    auto drawValue = [&] (juce::Rectangle<int> r, int steps)
    {
        g.setColour (stage);
        g.fillRoundedRectangle (r.toFloat(), radius);
        g.setColour (hairline);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), radius, 1.0f);
        g.setColour (steps != 0 ? accent : textDim);
        g.setFont (monoFont (17.0f).boldened());
        g.drawText (formatSteps (steps), r, juce::Justification::centred, false);
    };
    drawValue (toneValue, live.getToneSteps());
    drawValue (keyValue,  live.getKeyOffset());

    //== Key / scale readout: the centrepiece of the panel ===================================
    if (readout.getWidth() > 60)
    {
        auto r = readout.toFloat();
        const bool has = live.hasKey();

        // soft glow around the box
        g.setColour (accent.withAlpha (loading ? 0.06f : 0.12f));
        g.fillRoundedRectangle (r.expanded (2.5f), radiusLg + 2.5f);

        // blue tinted body, a little lighter at the top
        juce::ColourGradient body (accent.withAlpha (0.20f), 0.0f, r.getY(),
                                   accent.withAlpha (0.06f), 0.0f, r.getBottom(), false);
        g.setColour (stage);
        g.fillRoundedRectangle (r, radiusLg);
        g.setGradientFill (body);
        g.fillRoundedRectangle (r, radiusLg);

        // bright accent border; while loading it breathes
        const float pulse = 0.45f + 0.45f * (0.5f + 0.5f * std::sin ((float) (nowMs % 1200) / 1200.0f * twoPi));
        const float baseA = has ? 0.90f : 0.55f;
        g.setColour (loading ? accent.withAlpha (pulse)
                             : overReadout ? accentHover : accent.withAlpha (baseA));
        g.drawRoundedRectangle (r.reduced (0.8f), radiusLg, 1.6f);

        // One line only, centred in the box (horizontally and vertically): analysing animation,
        // error / status text, or the key. There is no separate status line any more.

        if (loading)
        {
            // "Analysing" look: top row = flowing, layered sine waves (fade out at both ends) with a soft
            // scanning glow moving over them; bottom row = the label with a light band sweeping over it.
            const float tSec = (float) (nowMs % 600000) / 1000.0f;
            const juce::Rectangle<float> waveR ((float) readout.getX() + 12.0f, (float) readout.getY() + 6.0f,
                                                (float) readout.getWidth() - 24.0f, 24.0f);
            const float cy = waveR.getCentreY();

            g.saveState();
            g.reduceClipRegion (waveR.getSmallestIntegerContainer());

            // faint centre line
            g.setColour (accent.withAlpha (0.18f));
            g.fillRect (waveR.getX(), cy - 0.5f, waveR.getWidth(), 1.0f);

            // soft scanning glow
            {
                const float sw    = waveR.getWidth() * 0.35f;
                const float phase = std::fmod (tSec * 0.75f, 1.5f) - 0.25f;       // sweeps a bit past both edges
                const float gx    = waveR.getX() + phase * waveR.getWidth() - sw * 0.5f;
                juce::ColourGradient glow (accent.withAlpha (0.0f), gx, 0.0f, accent.withAlpha (0.0f), gx + sw, 0.0f, false);
                glow.addColour (0.5, accent.withAlpha (0.28f));
                g.setGradientFill (glow);
                g.fillRect (waveR);
            }

            // three layered waves: front one bright and thick, back ones thinner and fainter
            juce::ColourGradient edge (accent.withAlpha (0.0f), waveR.getX(), 0.0f,
                                       accent.withAlpha (0.0f), waveR.getRight(), 0.0f, false);
            for (int layer = 2; layer >= 0; --layer)
            {
                const float freq  = 1.5f + 0.55f * (float) layer;
                const float speed = 2.6f + 1.1f * (float) layer;
                const float amp   = waveR.getHeight() * 0.5f * (0.92f - 0.2f * (float) layer);
                const float breathe = 0.6f + 0.4f * std::sin (tSec * 1.6f + (float) layer * 1.3f);

                juce::Path p;
                const float stepPx = 2.0f;
                for (float x = 0.0f; x <= waveR.getWidth(); x += stepPx)
                {
                    const float u   = x / waveR.getWidth();
                    const float env = std::pow (std::sin (u * juce::MathConstants<float>::pi), 1.5f);
                    const float y   = cy + std::sin (u * twoPi * freq - tSec * speed + (float) layer * 1.4f)
                                           * amp * env * breathe;
                    if (x == 0.0f) p.startNewSubPath (waveR.getX(), y);
                    else           p.lineTo (waveR.getX() + x, y);
                }

                const float a = layer == 0 ? 1.0f : (layer == 1 ? 0.55f : 0.32f);
                juce::ColourGradient grad (edge);
                grad.clearColours();
                grad.addColour (0.0, accentHover.withAlpha (0.0f));
                grad.addColour (0.5, (layer == 0 ? accentHover : accent).withAlpha (a));
                grad.addColour (1.0, accentHover.withAlpha (0.0f));
                g.setGradientFill (grad);
                g.strokePath (p, juce::PathStrokeType (layer == 0 ? 2.0f : 1.3f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            }
            g.restoreState();

            // label row (shrinks when the box is narrow), running dots, light band sweeping over it
            const auto label = juce::String::fromUTF8 ("Đang phân tích key/scale");
            const int  dots  = 1 + (int) ((nowMs % 1500) / 500);
            const int  availW = readout.getWidth() - 16;
            float lSize = szLabel;
            auto  lf    = font (lSize, true);
            while (lSize > 8.5f && juce::GlyphArrangement::getStringWidthInt (lf, label + "...") > availW)
            {
                lSize -= 0.5f;
                lf = font (lSize, true);
            }
            const int tw = juce::GlyphArrangement::getStringWidthInt (lf, label + "...");     // fixed width: no jitter
            const juce::Rectangle<int> grp (readout.getCentreX() - tw / 2, readout.getBottom() - 7 - 16, tw + 2, 16);
            const auto txt = label + juce::String::repeatedString (".", dots);

            g.setFont (lf);
            g.setColour (text.withAlpha (0.55f));
            g.drawText (txt, grp, juce::Justification::centredLeft, false);

            {
                const float sweep = (float) (nowMs % 1600) / 1600.0f;
                const float bw    = 46.0f;
                const float bx    = (float) grp.getX() - bw + ((float) tw + bw) * sweep;
                g.saveState();
                g.reduceClipRegion (juce::Rectangle<int> ((int) bx, grp.getY(), (int) bw, grp.getHeight()));
                juce::ColourGradient lg (text.withAlpha (0.0f), bx, 0.0f, text.withAlpha (0.0f), bx + bw, 0.0f, false);
                lg.addColour (0.5, text);
                g.setGradientFill (lg);
                g.drawText (txt, grp, juce::Justification::centredLeft, false);
                g.restoreState();
            }
        }
        else if (statusText.isNotEmpty() && (statusKind == StatusKind::Warn || statusKind == StatusKind::Error))
        {
            // error / warning: shown right where "Đang phân tích key/scale" was
            g.setColour (statusKind == StatusKind::Error ? danger : warning);
            g.setFont (font (szSmall + 1.5f, true));
            if (retryAvailable)
            {
                // short message on top, THỬ LẠI button under it
                g.drawFittedText (statusText, { readout.getX() + 10, readout.getY() + 6, readout.getWidth() - 20, 18 },
                                  juce::Justification::centred, 1, 0.7f);

                const auto pill = retryPillBounds().toFloat();
                g.setColour (accent.withAlpha (overRetry ? 0.55f : 0.28f));
                g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
                g.setColour (overRetry ? accentHover : accent);
                g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.2f);
                g.setColour (text);
                g.setFont (font (szMicro, true));
                g.drawText (juce::String::fromUTF8 ("THỬ LẠI"), pill, juce::Justification::centred, false);
            }
            else
            {
                g.drawFittedText (statusText, readout.reduced (12, 3), juce::Justification::centred, 2, 0.85f);
            }
        }
        else
        {
            // result: blinks for 2 s (4 flashes) right after it is found, then stays solid
            float a = 1.0f;
            if (flashing)
            {
                const auto el = nowMs - flashStartMs;
                if (el < 2000)
                    a = ((el / 250) % 2 == 0) ? 1.0f : 0.12f;
            }
            g.setColour ((has ? text : textFaint).withAlpha (a));
            if (has)
            {
                // Two lines, centred as one block: "A MINOR" (big) and its Vietnamese name "LA THỨ" below it
                // (smaller, other colour). The Vietnamese line shrinks if it is wider than the box.
                const auto en = live.getFinalRootName();
                const auto sc = live.getScaleName();
                const auto vi = LiveController::vietnameseKeyName (en, sc);

                constexpr int mainH = 26, viH = 15;
                auto block = readout.withSizeKeepingCentre (readout.getWidth(), mainH + viH);
                auto mainR = block.removeFromTop (mainH);
                auto viR   = block;

                const auto mainTxt = en + " " + sc.toUpperCase();
                float mainSize = 22.0f;
                auto  mainF    = font (mainSize, true);
                while (mainSize > 11.0f && juce::GlyphArrangement::getStringWidthInt (mainF, mainTxt) > readout.getWidth() - 16)
                {
                    mainSize -= 0.5f;
                    mainF = font (mainSize, true);
                }
                g.setFont (mainF);
                g.drawText (mainTxt, mainR, juce::Justification::centred, true);

                float viSize = 13.5f;
                auto  viF    = font (viSize, true);
                const int maxW = readout.getWidth() - 20;
                while (viSize > 8.0f && juce::GlyphArrangement::getStringWidthInt (viF, vi) > maxW)
                {
                    viSize -= 0.5f;
                    viF = font (viSize, true);
                }
                g.setColour (accentHover.withAlpha (a));
                g.setFont (viF);
                g.drawText (vi, viR, juce::Justification::centred, true);
            }
            else
            {
                g.setFont (font (22.0f, true));
                g.drawText (juce::String (juce::CharPointer_UTF8 ("\xE2\x80\x94  \xE2\x80\x94")),
                            readout, juce::Justification::centred, true);
            }
        }
    }
}
