/*
    pt.Live - Live Singing DAW
    PtLiveLookAndFeel - flat, borderless, near-monochrome controls (spec section 10).

    Installed once with LookAndFeel::setDefaultLookAndFeel() from MainApplication.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Theme.h"

//==============================================================================
// Window button (minimise / maximise / close) drawn exactly like the main app's title bar:
// dim glyph, hover brightens it, close turns red with a white X.
class PtLiveWindowButton : public juce::Button
{
public:
    enum class Kind { minimise, maximise, close };

    explicit PtLiveWindowButton (Kind k)
        : juce::Button (k == Kind::close ? "close" : k == Kind::maximise ? "maximise" : "minimise"), kind (k)
    {
        setWantsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        using namespace ptl::theme;

        const bool isClose = kind == Kind::close;
        auto b = getLocalBounds();

        if (over && isClose)
        {
            g.setColour (down ? danger.darker (0.15f) : danger);
            g.fillRect (b);
        }
        else if (over)
        {
            g.setColour (juce::Colours::white.withAlpha (down ? 0.09f : 0.05f));
            g.fillRect (b);
        }

        g.setColour (over && isClose ? textOnAccent : over ? ptl::theme::text : textDim);

        const auto c = b.getCentre().toFloat();

        switch (kind)
        {
            case Kind::minimise:
                g.drawLine (c.x - 5.0f, c.y, c.x + 5.0f, c.y, 1.0f);
                break;
            case Kind::maximise:
                g.drawRoundedRectangle (juce::Rectangle<float> (9.5f, 9.5f).withCentre (c), 1.5f, 1.0f);
                break;
            case Kind::close:
            {
                const float s = 4.5f;
                g.drawLine (c.x - s, c.y - s, c.x + s, c.y + s, 1.1f);
                g.drawLine (c.x + s, c.y - s, c.x - s, c.y + s, 1.1f);
                break;
            }
        }
    }

private:
    Kind kind;
};

class PtLiveLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PtLiveLookAndFeel()
    {
        using namespace ptl::theme;

        if (ptl::theme::detail::face().name.isNotEmpty())
            setDefaultSansSerifTypefaceName (ptl::theme::detail::face().name);

        setColour (juce::ResizableWindow::backgroundColourId, window);
        setColour (juce::DocumentWindow::textColourId,        text);

        setColour (juce::TextButton::buttonColourId,   raised);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId,  text);
        setColour (juce::TextButton::textColourOnId,   textOnAccent);

        setColour (juce::ComboBox::backgroundColourId, raised);
        setColour (juce::ComboBox::outlineColourId,    outline);
        setColour (juce::ComboBox::textColourId,       text);
        setColour (juce::ComboBox::arrowColourId,      textDim);
        setColour (juce::ComboBox::focusedOutlineColourId, accent);

        setColour (juce::PopupMenu::backgroundColourId,            raised);
        setColour (juce::PopupMenu::textColourId,                  text);
        setColour (juce::PopupMenu::headerTextColourId,            textDim);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, selection);
        setColour (juce::PopupMenu::highlightedTextColourId,       text);

        setColour (juce::Label::textColourId, text);
        setColour (juce::Label::textWhenEditingColourId, text);
        setColour (juce::TextEditor::backgroundColourId,     raisedHi);
        setColour (juce::TextEditor::textColourId,           text);
        setColour (juce::TextEditor::outlineColourId,        outline);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::TextEditor::highlightColourId,      accent.withAlpha (0.35f));
        setColour (juce::CaretComponent::caretColourId,      text);

        setColour (juce::Slider::rotarySliderFillColourId,    accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, stage);
        setColour (juce::Slider::thumbColourId,               text);
        setColour (juce::Slider::trackColourId,               accent);
        setColour (juce::Slider::backgroundColourId,          stage);
        setColour (juce::Slider::textBoxTextColourId,         text);
        setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);

        setColour (juce::ScrollBar::thumbColourId, outlineStrong);

        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, accent);

        setColour (juce::AlertWindow::backgroundColourId, panel);
        setColour (juce::AlertWindow::textColourId,       text);
        setColour (juce::AlertWindow::outlineColourId,    outline);

        setColour (juce::TooltipWindow::backgroundColourId, raised);
        setColour (juce::TooltipWindow::textColourId,       text);
        setColour (juce::TooltipWindow::outlineColourId,    outline);

        setColour (juce::ListBox::backgroundColourId, stage);
        setColour (juce::ListBox::outlineColourId,    outline);
        setColour (juce::ProgressBar::backgroundColourId, stage);
        setColour (juce::ProgressBar::foregroundColourId, accent);
    }

    //== Fonts ================================================================
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return ptl::theme::font (juce::jmin (ptl::theme::szBody, (float) buttonHeight * 0.6f / 0.65f));
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        const float custom = (float) (double) box.getProperties().getWithDefault ("fontHeight", 0.0);
        const bool  warn   = (bool) box.getProperties().getWithDefault ("warn", false);      // "No Input" etc.
        if (custom > 0.0f)
            return ptl::theme::font (custom, warn);

        return ptl::theme::font (juce::jmin (ptl::theme::szBody, (float) box.getHeight() * 0.6f / 0.65f));
    }

    juce::Font getPopupMenuFont() override        { return ptl::theme::font (ptl::theme::szBody); }
    juce::Font getAlertWindowTitleFont() override { return ptl::theme::font (ptl::theme::szTitle, true); }
    juce::Font getAlertWindowMessageFont() override { return ptl::theme::font (ptl::theme::szBody); }
    juce::Font getAlertWindowFont() override      { return ptl::theme::font (ptl::theme::szBody); }

    juce::Font getLabelFont (juce::Label& label) override
    {
        auto f = label.getFont();

        // Fonts made by ptl::theme::font() already carry the right face and scale.
        if (f.getTypefaceName() != ptl::theme::detail::face().name)
            f = ptl::theme::font (f.getHeight(), f.isBold());

        return f;
    }

    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* l = juce::LookAndFeel_V4::createSliderTextBox (slider);
        l->setFont (ptl::theme::font (ptl::theme::szSmall));
        return l;
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 30, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    //== Buttons ==============================================================
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override
    {
        using namespace ptl::theme;

        auto fill = backgroundColour;

        if (down)             fill = fill.brighter (0.12f);
        else if (highlighted) fill = fill.brighter (0.06f);
        if (! button.isEnabled())
            fill = fill.withMultipliedAlpha (0.5f);

        auto b = button.getLocalBounds().toFloat();
        g.setColour (fill);
        g.fillRoundedRectangle (b, juce::jmin (radius, b.getHeight() * 0.25f));

        if (button.hasKeyboardFocus (true))
        {
            g.setColour (accent.withAlpha (0.6f));
            g.drawRoundedRectangle (b.reduced (0.5f), juce::jmin (radius, b.getHeight() * 0.25f), 1.0f);
        }
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool) override
    {
        using namespace ptl::theme;

        constexpr float s = 16.0f;
        auto b = button.getLocalBounds().toFloat();
        const bool hasText = button.getButtonText().isNotEmpty();

        auto box = hasText ? juce::Rectangle<float> (b.getX() + 2.0f, b.getCentreY() - s * 0.5f, s, s)
                           : juce::Rectangle<float> (s, s).withCentre (b.getCentre());

        g.setColour (stage.withAlpha (highlighted ? 0.9f : 0.6f));
        g.fillRoundedRectangle (box, 3.5f);
        g.setColour (outline);
        g.drawRoundedRectangle (box.reduced (0.5f), 3.5f, 1.0f);

        if (button.getToggleState())
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 3.6f,  box.getCentreY() + 0.2f);
            tick.lineTo          (box.getX() + 6.8f,  box.getCentreY() + 3.2f);
            tick.lineTo          (box.getRight() - 3.4f, box.getCentreY() - 3.2f);
            g.setColour (button.findColour (juce::ToggleButton::tickColourId));
            g.strokePath (tick, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }

        if (hasText)
        {
            g.setColour (button.findColour (juce::ToggleButton::textColourId)
                               .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
            g.setFont (font (szBody));
            g.drawText (button.getButtonText(), box.getRight() + 10.0f, b.getY(),
                        b.getRight() - box.getRight() - 10.0f, b.getHeight(),
                        juce::Justification::centredLeft, true);
        }
    }

    //== ComboBox =============================================================
    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int,
                       juce::ComboBox& box) override
    {
        using namespace ptl::theme;

        auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
        const bool focus = box.hasKeyboardFocus (true);

        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (b, radius);

        const bool warn = (bool) box.getProperties().getWithDefault ("warn", false);

        g.setColour (focus ? accent : box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (b, radius, (focus || warn) ? 1.5f : 1.0f);

        const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
        juce::Path chevron;
        chevron.startNewSubPath (cx - 4.5f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.5f);
        chevron.lineTo (cx + 4.5f, cy - 2.0f);

        g.setColour (box.findColour (juce::ComboBox::arrowColourId)
                        .withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    //== Sliders ==============================================================
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        using namespace ptl::theme;

        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (3.0f);
        const float r      = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto  centre = bounds.getCentre();
        const float lineW  = juce::jmax (2.5f, r * 0.13f);
        const float arcR   = r - lineW * 0.5f;
        const bool active  = slider.isMouseOverOrDragging() && slider.isEnabled();
        const float angle  = startAngle + pos * (endAngle - startAngle);

        const juce::PathStrokeType stroke (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        // groove
        juce::Path groove;
        groove.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
        g.strokePath (groove, stroke);

        // value arc (unipolar from the start, bipolar from 12 o'clock)
        const bool bipolar = (bool) slider.getProperties().getWithDefault ("bipolar", false);
        const float from   = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

        if (std::abs (angle - from) > 0.01f)
        {
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f,
                               juce::jmin (from, angle), juce::jmax (from, angle), true);
            auto c = slider.findColour (juce::Slider::rotarySliderFillColourId);
            g.setColour (active ? c.brighter (0.25f) : c);
            g.strokePath (arc, stroke);
        }

        // unity tick, just outside the ring
        if (slider.getProperties().contains ("unityPos"))
        {
            const float up = startAngle + (float) (double) slider.getProperties()["unityPos"] * (endAngle - startAngle);
            const float a  = up - juce::MathConstants<float>::halfPi;
            g.setColour (textDim);
            g.drawLine (centre.x + std::cos (a) * (r + 0.5f), centre.y + std::sin (a) * (r + 0.5f),
                        centre.x + std::cos (a) * (r + 2.5f), centre.y + std::sin (a) * (r + 2.5f), 1.2f);
        }

        // disc + pointer
        const float discR = juce::jmax (4.0f, arcR - lineW * 0.5f - 3.0f);
        g.setColour (raisedHi);
        g.fillEllipse (centre.x - discR, centre.y - discR, discR * 2.0f, discR * 2.0f);
        g.setColour (outline);
        g.drawEllipse (centre.x - discR + 0.5f, centre.y - discR + 0.5f, discR * 2.0f - 1.0f, discR * 2.0f - 1.0f, 1.0f);

        const float pa = angle - juce::MathConstants<float>::halfPi;
        g.setColour (slider.isEnabled() ? text : textFaint);
        g.drawLine (centre.x + std::cos (pa) * discR * 0.25f, centre.y + std::sin (pa) * discR * 0.25f,
                    centre.x + std::cos (pa) * (discR - 1.5f), centre.y + std::sin (pa) * (discR - 1.5f),
                    2.0f);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                           juce::Slider& slider) override
    {
        if (slider.isBar())
        {
            auto b = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
            const float rad = b.getHeight() * 0.5f;

            g.setColour (slider.findColour (juce::Slider::backgroundColourId));
            g.fillRoundedRectangle (b, rad);

            auto fill = b.withRight (juce::jlimit (b.getX(), b.getRight(), sliderPos));
            if (fill.getWidth() > 1.0f)
            {
                g.setColour (slider.findColour (juce::Slider::trackColourId));
                g.fillRoundedRectangle (fill, rad);
            }
            return;
        }

        if (style == juce::Slider::LinearHorizontal)
        {
            using namespace ptl::theme;
            const float cy = (float) y + (float) height * 0.5f;
            auto groove = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) width, 4.0f);

            g.setColour (slider.findColour (juce::Slider::backgroundColourId));
            g.fillRoundedRectangle (groove, 2.0f);
            g.setColour (slider.findColour (juce::Slider::trackColourId));
            g.fillRoundedRectangle (groove.withRight (sliderPos), 2.0f);

            if (slider.isEnabled())
            {
                g.setColour (slider.findColour (juce::Slider::thumbColourId));
                g.fillEllipse (sliderPos - 5.0f, cy - 5.0f, 10.0f, 10.0f);
            }
            return;
        }

        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos,
                                                maxSliderPos, style, slider);
    }

    //== Dialog title bar (same look as the main app's title bar) =============
    void drawDocumentWindowTitleBar (juce::DocumentWindow& window, juce::Graphics& g, int w, int h,
                                     int titleSpaceX, int titleSpaceW, const juce::Image* /*icon*/,
                                     bool /*drawTitleTextOnLeft*/) override
    {
        using namespace ptl::theme;

        if (w * h == 0)
            return;

        g.setColour (panel);
        g.fillRect (0, 0, w, h);

        g.setColour (hairline);
        g.fillRect (0, h - 1, w, 1);

        g.setColour (text);
        g.setFont (font (szBody, true));
        g.drawText (window.getName(), juce::Rectangle<int> (titleSpaceX + 16, 0, juce::jmax (0, titleSpaceW - 16), h),
                    juce::Justification::centredLeft, true);
    }

    juce::Button* createDocumentWindowButton (int buttonType) override
    {
        using Kind = PtLiveWindowButton::Kind;

        if (buttonType == juce::DocumentWindow::closeButton)    return new PtLiveWindowButton (Kind::close);
        if (buttonType == juce::DocumentWindow::minimiseButton) return new PtLiveWindowButton (Kind::minimise);
        if (buttonType == juce::DocumentWindow::maximiseButton) return new PtLiveWindowButton (Kind::maximise);

        return nullptr;
    }

    void positionDocumentWindowButtons (juce::DocumentWindow&, int titleBarX, int titleBarY, int titleBarW, int titleBarH,
                                        juce::Button* minimiseButton, juce::Button* maximiseButton, juce::Button* closeButton,
                                        bool positionTitleBarButtonsOnLeft) override
    {
        const int buttonW = 46;          // flush with the right edge, full title bar height (like the main app)
        int x = positionTitleBarButtonsOnLeft ? titleBarX : titleBarX + titleBarW - buttonW;
        const int step = positionTitleBarButtonsOnLeft ? buttonW : -buttonW;

        for (auto* b : { closeButton, maximiseButton, minimiseButton })
        {
            if (b == nullptr)
                continue;

            b->setBounds (x, titleBarY, buttonW, titleBarH);
            x += step;
        }
    }

    //== Scrollbar ============================================================
    int getDefaultScrollbarWidth() override { return 10; }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                        bool vertical, int thumbStart, int thumbSize, bool over, bool down) override
    {
        using namespace ptl::theme;

        if (thumbSize <= 0)
            return;

        auto thumb = vertical ? juce::Rectangle<float> ((float) x + 2.0f, (float) thumbStart, (float) width - 4.0f, (float) thumbSize)
                              : juce::Rectangle<float> ((float) thumbStart, (float) y + 2.0f, (float) thumbSize, (float) height - 4.0f);

        auto c = bar.findColour (juce::ScrollBar::thumbColourId);
        g.setColour (down ? accent : over ? c.brighter (0.3f) : c);
        g.fillRoundedRectangle (thumb, 3.0f);
    }

    //== Tooltip ==============================================================
    void drawTooltip (juce::Graphics& g, const juce::String& tip, int width, int height) override
    {
        using namespace ptl::theme;

        auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
        g.setColour (raised);
        g.fillRoundedRectangle (b, radius);
        g.setColour (outline);
        g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);

        g.setColour (text);
        g.setFont (font (szSmall));
        g.drawFittedText (tip, 6, 3, width - 12, height - 6, juce::Justification::centredLeft, 3);
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& tip, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override
    {
        juce::AttributedString s;
        s.setJustification (juce::Justification::centredLeft);
        s.append (tip, ptl::theme::font (ptl::theme::szSmall), ptl::theme::text);

        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths (s, 400.0f);

        const int w = (int) std::ceil (layout.getWidth())  + 16;
        const int h = (int) std::ceil (layout.getHeight()) + 10;

        return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                     screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)  : screenPos.y + 6,
                                     w, h).constrainedWithin (parentArea);
    }
};
