/*
    pt.Live - Live Singing DAW
    Custom dark title bar (42px): [License Plugins Devices File] + minimise / maximise / close.
    The text buttons are plain text: no frame, no fill, no focus ring (hover only brightens the text) and each one is
    only as wide as its text, so the whole cluster stays short and never reaches the brand on the left.
    The licence is shown as a small status next to Plugins - plan + time left ("Dùng thử · còn 5 ngày"), always visible
    (it gets shorter, never hidden, when the bar is narrow). Double-click it to enter / renew a key.
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>
#include "include/Theme.h"

//==============================================================================
// Animated brand block: [ECG gap]  pt.Live  [ECG gap]  |  033.247.3333
// The phone number stays at the right end of the strip. "pt.Live" is centred in the space to its left, so the two
// empty gaps on its left and right are equal and together form one ECG screen: a bright head sweeps left -> right
// over the left gap, then jumps straight over the wordmark to the start of the right gap and goes on. Each gap tucks
// a little (underLetterPx) beneath the neighbouring letter, the "p" on the left and the "e" on the right: the trace
// is drawn BEFORE the text, so it only shows between / inside the strokes and is never drawn over a letter. The line behind the head fades out, and a short gap is kept just ahead of the head
// where the old trace has been erased. Every beat is a P-QRS-T complex whose R wave is as tall as the master level; a
// new beat starts as soon as the previous one has finished while the master is above the gate, and a tiny idle beat
// keeps the line alive in silence (idleAmp = 0 -> dead flat line).
// It replaces the old master VU meter of the collapsed panel.
// Restrained on purpose: one cool palette (blue / teal / soft violet), slow motion.
class BrandMark : public juce::Component,
                  private juce::Timer
{
public:
    BrandMark()
    {
        setInterceptsMouseClicks (false, false);     // clicks / drags fall through to the TitleBar
        setOpaque (false);
    }

    ~BrandMark() override { stopTimer(); }

    // Width needed for mark + wordmark + phone with the current font (set by resized()).
    int getIdealWidth() const noexcept { return idealWidth; }

    /** Master level source. Called ~30x/s on the message thread while the title bar is showing; must return the
        loudest channel peak (dBFS) since the previous call, or <= -100 when there is nothing (no meter yet). */
    std::function<float()> levelProvider;

    void paint (juce::Graphics& g) override
    {
        using namespace ptl::theme;

        const double t = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const float cy = (float) getHeight() * 0.5f;

        //== ECG sweep over the left and the right gap (the wordmark in between is skipped) =====
        {
            const auto area   = traceBounds;
            const float baseY = area.getCentreY() + area.getHeight() * 0.14f;     // baseline a little below the middle: the R wave needs the room
            const float upRoom = baseY - area.getY() - 1.0f;                      // height of the R wave at 0 dBFS
            const int   n       = numSamples;
            const int   nl      = leftSamples;                                    // samples 0..nl-1 = left gap, nl..n-1 = right gap
            const int   visible = n - gapSamples;

            auto smooth = [] (float x) { x = juce::jlimit (0.0f, 1.0f, x); return x * x * (3.0f - 2.0f * x); };
            auto xOf    = [&] (int k) { return k < nl ? (float) k / samplesPerPx : rightStartX + (float) (k - nl) / samplesPerPx; };
            auto yOf    = [&] (int k) { return baseY - juce::jlimit (-0.6f, 1.0f, trace[(size_t) k]) * upRoom; };

            // Each gap fades in and out at both of its ends: 12 px at the outer ends (app edge / divider), innerFadePx at the
            // ends that tuck under the "p" / the "e": the trace is already dim when it reaches the letter and gone at its tip.
            auto edgeOf = [&] (int k)
            {
                const bool  left    = k < nl;
                const float len     = (float) (left ? nl : n - nl) / samplesPerPx;
                const float x       = (float) (left ? k : k - nl) / samplesPerPx;
                const float fadeIn  = left ? 12.0f : innerFadePx;
                const float fadeOut = left ? innerFadePx : 12.0f;
                return smooth (x / fadeIn) * smooth ((len - x) / fadeOut);
            };

            // Brightness of sample k: 1 at the head -> 0 at the end of the tail; 0 inside the gap.
            auto alphaOf = [&] (int k)
            {
                const int age = (sweepIdx - k + n) % n;                    // 0 = newest sample (the head)
                if (age >= visible)
                    return 0.0f;
                return std::pow (1.0f - (float) age / (float) visible, 1.6f) * edgeOf (k);
            };

            for (int k = 0; k < n - 1; ++k)
            {
                if (k == sweepIdx || k + 1 == nl)                          // newest -> oldest (the gap), or left gap -> right gap (the wordmark)
                    continue;

                const float a = 0.5f * (alphaOf (k) + alphaOf (k + 1));
                if (a < 0.01f)
                    continue;

                const float u = xOf (k) / juce::jmax (1.0f, (float) getWidth());
                const auto  c = flowColour (u * 0.45f + (float) t * 0.08f);

                g.setColour (c.withAlpha (a * 0.16f));                     // soft glow under the line
                g.drawLine (xOf (k), yOf (k), xOf (k + 1), yOf (k + 1), 3.4f);
                g.setColour (c.withAlpha (a * 0.95f));
                g.drawLine (xOf (k), yOf (k), xOf (k + 1), yOf (k + 1), 1.5f);
            }

            // The head: a small bright dot with a halo.
            const float he = edgeOf (sweepIdx);
            if (he > 0.01f)
            {
                const float hx = xOf (sweepIdx), hy = yOf (sweepIdx);
                const auto  hc = juce::Colours::white.interpolatedWith (flowColour (hx / juce::jmax (1.0f, (float) getWidth()) * 0.45f + (float) t * 0.08f), 0.35f);

                g.setColour (hc.withAlpha (0.22f * he));
                g.fillEllipse (hx - 4.0f, hy - 4.0f, 8.0f, 8.0f);
                g.setColour (hc.withAlpha (0.95f * he));
                g.fillEllipse (hx - 1.7f, hy - 1.7f, 3.4f, 3.4f);
            }
        }

        //== Wordmark "pt.Live" ================================================
        const float pulse = 0.5f + 0.5f * std::sin ((float) t * 1.6f);     // slow breathing, 0..1

        // "pt": plain bright text
        g.setColour (text);
        g.fillPath (ptPath);

        // ".": accent dot that breathes
        {
            auto c = accent.interpolatedWith (flowColour ((float) t * 0.08f), 0.5f);
            g.setColour (c.withAlpha (0.12f + 0.12f * pulse));
            g.strokePath (dotPath, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (c.withAlpha (0.70f + 0.30f * pulse));
            g.fillPath (dotPath);
        }

        // "Live": flowing gradient + glow + light sweep
        {
            const auto flow = makeFlow (livePath.getBounds().getX(), livePath.getBounds().getWidth(), t);

            g.setGradientFill (flow);
            g.setOpacity (0.10f + 0.10f * pulse);
            g.strokePath (livePath, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setGradientFill (flow);
            g.fillPath (livePath);

            drawSweep (g, livePath, t, 0.0, 0.55f);
        }

        //== Divider + phone ===================================================
        {
            g.setColour (outlineStrong);
            g.fillRect (juce::Rectangle<float> (dividerX, cy - 9.0f, 1.0f, 18.0f));

            // Quiet by default; the sweep (slightly later than the logo) lifts it for a moment.
            g.setColour (textDim);
            g.fillPath (phonePath);
            drawSweep (g, phonePath, t, 0.45, 0.45f);
        }
    }

    void resized() override
    {
        using namespace ptl::theme;

        const float cy = (float) getHeight() * 0.5f;
        const float W  = (float) getWidth();            // the whole brand strip (starts at x = 0, see TitleBar::resized)

        // Vertical room of the ECG: up to 30 px, centred.
        markBounds = juce::Rectangle<int> (0, 0, getWidth(), juce::jmin (30, getHeight())).withCentre ({ getWidth() / 2, getHeight() / 2 });

        const auto nameFont  = spacedFont (25.0f, 0.02f, true);
        const auto phoneFont = font (13.0f, false).withExtraKerningFactor (0.05f);

        // 1) Phone number at the right end of the strip, divider just before it.
        float phoneW = 0.0f;
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (phoneFont, "033.247.3333", 0.0f, 0.0f);
            phoneW = ga.getBoundingBox (0, -1, true).getWidth();
        }

        const float phoneX = W - 6.0f - phoneW;
        phonePath.clear();
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (phoneFont, "033.247.3333", phoneX, cy + phoneFont.getHeight() * 0.30f);
            ga.createPath (phonePath);
        }

        dividerX = phoneX - 14.0f;
        const float rightEnd = dividerX - 8.0f;          // the ECG ends here (a little before the divider)

        // 2) Wordmark: large bold, slightly tracked. Built at x = 0, then moved so that its visible letters (the "ink")
        //    sit in the middle between the left edge of the app and rightEnd: the two empty gaps are then equal.
        ptPath.clear(); dotPath.clear(); livePath.clear();
        const float baseline = cy + nameFont.getHeight() * 0.30f;
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (nameFont, "pt.Live", 0.0f, baseline);

            for (int i = 0; i < ga.getNumGlyphs(); ++i)
            {
                juce::Path p;
                ga.getGlyph (i).createPath (p);

                if (i < 2)       ptPath.addPath (p);
                else if (i == 2) dotPath.addPath (p);
                else             livePath.addPath (p);
            }
        }

        {
            const float inkL0 = ptPath.getBounds().getX();
            const float inkR0 = livePath.getBounds().getRight();
            const auto  move  = juce::AffineTransform::translation (std::round ((rightEnd - (inkR0 - inkL0)) * 0.5f - inkL0), 0.0f);

            ptPath.applyTransform (move);
            dotPath.applyTransform (move);
            livePath.applyTransform (move);
        }

        const float inkL = ptPath.getBounds().getX();         // left edge of the "p"
        const float inkR = livePath.getBounds().getRight();   // right edge of the "e"

        // 3) The two ECG gaps (same length): [0 .. inkL + underLetterPx] and [inkR - underLetterPx .. rightEnd].
        //    They reach underLetterPx into the "p" and into the "e"; the text is painted after the trace.
        const float leftLen = juce::jmax (0.0f, inkL + underLetterPx);
        rightStartX = inkR - underLetterPx;
        const float rightLen = juce::jmax (0.0f, rightEnd - rightStartX);

        traceBounds = markBounds.toFloat();

        const int nl   = (int) (leftLen  * (float) samplesPerPx);
        const int nr   = (int) (rightLen * (float) samplesPerPx);
        const int newN = juce::jmax (gapSamples + 8, nl + nr);

        if (newN != numSamples || nl != leftSamples)
        {
            leftSamples = nl;
            numSamples  = newN;
            trace.assign ((size_t) newN, 0.0f);
            sweepIdx = 0;
        }

        idealWidth = getWidth();
    }

    void visibilityChanged() override
    {
        if (isShowing())
        {
            lastTickMs = juce::Time::getMillisecondCounterHiRes();   // do not "catch up" for the time the window was hidden
            startTimerHz (30);                                        // the head moves ~1.7 px per tick: 30 Hz keeps it smooth
        }
        else
        {
            stopTimer();
        }
    }

    void parentHierarchyChanged() override { visibilityChanged(); }

private:
    void timerCallback() override
    {
        if (numSamples <= 0)
            return;

        const double now  = juce::Time::getMillisecondCounterHiRes();
        const double dtMs = juce::jlimit (0.0, 100.0, now - lastTickMs);      // stall / hidden window: do not jump
        lastTickMs = now;

        // Master level: dB -> 0..1 (-60 dBFS and below = 0). 1.3: whispers stay small, a voice makes tall beats.
        float lv = 0.0f;

        if (levelProvider)
        {
            const float db = levelProvider();

            if (db > -60.0f)
                lv = std::pow (juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f), 1.3f);
        }

        // Until its R wave has been drawn, the running beat keeps growing with the loudest master reading.
        if (beatPx < rPeakPx)
            beatAmp = juce::jmax (beatAmp, lv);

        // Move the head: whole samples only, the remainder is kept so the speed does not depend on the timer rate.
        sweepFrac += (float) (dtMs * 0.001 * sweepPxPerSec) * samplesPerPx;
        const int steps = (int) sweepFrac;
        sweepFrac -= (float) steps;

        for (int s = 0; s < steps; ++s)
        {
            if (beatPx >= beatSpanPx)                                         // previous beat finished: next one?
            {
                if (lv > gate)                                   startBeat (lv);
                else if (idleAmp > 0.0f && beatPx >= idleSpanPx) startBeat (idleAmp);
            }

            beatPx = juce::jmin (beatPx + 1.0f / samplesPerPx, 1000.0f);
            sweepIdx = (sweepIdx + 1) % numSamples;
            trace[(size_t) sweepIdx] = beatPx < beatSpanPx ? beatAmp * ecgShape (beatPx) : 0.0f;
        }

        repaint();
    }

    void startBeat (float amp) noexcept
    {
        beatPx  = 0.0f;
        beatAmp = juce::jmax (amp, 0.0f);
    }

    // One P-QRS-T complex, x in px from the start of the beat (~24 px = 0.45 s at the default sweep speed).
    // Returns about -0.3 .. 1.0 (R wave = 1.0).
    static float ecgShape (float x) noexcept
    {
        auto gauss = [] (float v, float c, float w) { const float d = (v - c) / w; return std::exp (-0.5f * d * d); };

        return  0.13f * gauss (x,  3.5f, 2.0f)      // P
              - 0.14f * gauss (x,  8.6f, 0.7f)      // Q
              + 1.00f * gauss (x, 10.0f, 0.85f)     // R
              - 0.30f * gauss (x, 11.5f, 0.8f)      // S
              + 0.24f * gauss (x, 17.0f, 2.6f);     // T
    }

    // Brand palette: calm blue -> teal -> soft violet. Cyclic, `p` is in cycles.
    static juce::Colour flowColour (float p)
    {
        static const juce::Colour pal[3] { juce::Colour (0xff5aa9e6), juce::Colour (0xff4fd1c0), juce::Colour (0xffa392f4) };
        p -= std::floor (p);
        const float f = p * 3.0f;
        const int   i = (int) f;
        return pal[i % 3].interpolatedWith (pal[(i + 1) % 3], f - (float) i);
    }

    // "Live" is red: a slow flow between a deep red, a bright red and a soft coral red. Cyclic, `p` is in cycles.
    static juce::Colour liveColour (float p)
    {
        static const juce::Colour pal[3] { juce::Colour (0xffd81b34), juce::Colour (0xffff3b4e), juce::Colour (0xffff6f6f) };
        p -= std::floor (p);
        const float f = p * 3.0f;
        const int   i = (int) f;
        return pal[i % 3].interpolatedWith (pal[(i + 1) % 3], f - (float) i);
    }

    // Horizontal gradient that is periodic with period L and drifts right; 7 s per cycle. Red, for "Live".
    static juce::ColourGradient makeFlow (float x0, float width, double t)
    {
        const float L     = juce::jmax (1.0f, width * 1.6f);
        const float phase = (float) std::fmod (t / 7.0, 1.0);
        const float sx    = x0 - L * (2.0f - phase);

        juce::ColourGradient gr (liveColour (0.0f), sx, 0.0f, liveColour (0.0f), sx + 3.0f * L, 0.0f, false);

        for (int cycle = 0; cycle < 3; ++cycle)
            for (int k = 0; k < 3; ++k)
            {
                const double pos = ((double) cycle + (double) k / 3.0) / 3.0;
                if (pos > 0.0 && pos < 1.0)
                    gr.addColour (pos, liveColour ((float) k / 3.0f));
            }

        return gr;
    }

    // A soft diagonal highlight that crosses the glyphs for ~1.1 s every ~5.2 s.
    static void drawSweep (juce::Graphics& g, const juce::Path& glyphs, double t, double delay, float strength)
    {
        constexpr double period = 5.2, travel = 1.1;
        const double u = std::fmod (t - delay + period * 100.0, period) / travel;
        if (u >= 1.0 || glyphs.isEmpty())
            return;

        const auto b = glyphs.getBounds();
        const float bandX = b.getX() - 24.0f + (float) u * (b.getWidth() + 48.0f);

        juce::ColourGradient gr (juce::Colours::white.withAlpha (0.0f), bandX - 20.0f, b.getY(),
                                 juce::Colours::white.withAlpha (0.0f), bandX + 20.0f, b.getBottom(), false);
        gr.addColour (0.5, juce::Colours::white.withAlpha (strength));

        g.saveState();
        g.reduceClipRegion (glyphs, juce::AffineTransform());
        g.setGradientFill (gr);
        g.fillRect (b.expanded (2.0f));
        g.restoreState();
    }

    // ECG: the wordmark is centred in the space left of the phone number; the trace is one timeline made of the left gap followed by the right gap
    // (the wordmark is skipped), sampled every 1 / samplesPerPx px. resized() fills in numSamples / leftSamples / rightStartX.
    static constexpr int    samplesPerPx = 2;
    static constexpr float  underLetterPx = 7.0f;        // how far each gap reaches under the "p" (left) / the "e" (right). 0 = stop at the letters.
    static constexpr float  innerFadePx   = underLetterPx + 10.0f;   // the trace fades out over this length towards the letters: ~35 % bright at the letter edge, 0 at its tip, so "pt.Live" always stays the star
    static constexpr int    gapSamples   = 12;           // erased gap just ahead of the head (6 px)
    static constexpr float  sweepPxPerSec = 70.0f;       // px per second along the two gaps together. Bigger = faster sweep.
    static constexpr float  beatSpanPx   = 24.0f;        // length of one P-QRS-T complex
    static constexpr float  rPeakPx      = 10.0f;        // where the R wave sits inside the beat (the height is fixed there)
    static constexpr float  idleSpanPx   = 64.0f;        // in silence: one tiny beat every ~1.2 s
    static constexpr float  idleAmp      = 0.14f;        // its height; 0 = a dead flat line in silence
    static constexpr float  gate         = 0.12f;        // master level (0..1) above which beats are back to back (~ -51 dBFS)

    int    numSamples  = 300;                            // kept > 0 so the timer is always safe; resized() sets the real value
    int    leftSamples = 150;                            // samples that belong to the left gap
    std::vector<float> trace = std::vector<float> (300, 0.0f);   // ring buffer, 1.0 = R wave at 0 dBFS
    int    sweepIdx  = 0;                                // index of the newest sample = the head
    float  sweepFrac = 0.0f;                             // fraction of a sample not yet drawn
    float  beatPx    = beatSpanPx;                       // px since the running beat started (>= beatSpanPx: no beat running)
    float  beatAmp   = 0.0f;                             // height of the running beat, 0..1
    double lastTickMs = 0.0;
    juce::Rectangle<int> markBounds;
    juce::Rectangle<float> traceBounds;
    float rightStartX = 0.0f;                            // x where the right gap starts
    juce::Path ptPath, dotPath, livePath, phonePath;
    float dividerX = 0.0f;
    int   idealWidth = 300;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrandMark)
};

// Plain text button for the title bar: no frame, no fill, no focus ring - only the label. Hover / press just
// brighten the text (like the minimise / maximise / close glyphs). It never takes keyboard focus either, so a click
// cannot leave a ring around it or steal Ctrl+S / Ctrl+O from the main window.
class TitleTextButton : public juce::TextButton
{
public:
    explicit TitleTextButton (const juce::String& buttonText) : juce::TextButton (buttonText)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const float alpha = ! isEnabled() ? 0.40f : (isMouseOver || isButtonDown) ? 1.0f : 0.78f;

        g.setColour (ptl::theme::text.withMultipliedAlpha (alpha));
        g.setFont (ptl::theme::font (ptl::theme::szBody));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TitleTextButton)
};

// Licence status on the title bar:  "● Dùng thử · còn 5 ngày". Always visible. DOUBLE-click = enter / renew a key.
// When the bar is narrow it switches to the short form "● còn 5 ngày" instead of disappearing.
class LicenseChip : public juce::Component,
                    public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    enum class Level { trial, ok, warn, danger };

    LicenseChip() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    ~LicenseChip() override { stopTimer(); }

    std::function<void()> onDoubleClick;

    void setContent (const juce::String& newLongText, const juce::String& newShortText, Level newLevel)
    {
        if (longText == newLongText && shortText == newShortText && level == newLevel)
            return;

        longText  = newLongText;
        shortText = newShortText;
        level     = newLevel;

        // Close to the end (or over): the dot + text pulse so the user cannot miss it. danger = faster.
        if (level == Level::warn || level == Level::danger)
        {
            if (! isTimerRunning())
                startTimerHz (30);
        }
        else
        {
            stopTimer();
            pulse = 0.0f;
        }

        repaint();
    }

    void setShortForm (bool shouldUseShort)
    {
        if (useShort == shouldUseShort)
            return;

        useShort = shouldUseShort;
        repaint();
    }

    int getIdealWidth (bool shortForm) const
    {
        return dotSize + leftPad + 6 + juce::GlyphArrangement::getStringWidthInt (chipFont(), shortForm ? shortText : longText);
    }

    void paint (juce::Graphics& g) override
    {
        juce::Colour tint = ptl::theme::textDim;
        juce::Colour dot  = ptl::theme::success;

        switch (level)
        {
            case Level::trial:  dot = ptl::theme::accent;  break;
            case Level::ok:     dot = ptl::theme::success; break;
            case Level::warn:   dot = ptl::theme::warning; tint = ptl::theme::warning; break;
            case Level::danger: dot = ptl::theme::danger;  tint = ptl::theme::danger;  break;
        }

        if (hover && isEnabled())
            tint = tint.brighter (0.35f);                  // hover = brighter text, no box

        const bool alerting = (level == Level::warn || level == Level::danger);
        const float k       = alerting ? 0.5f + 0.5f * std::sin (pulse) : 0.0f;       // 0..1, smooth
        const float alpha   = (isEnabled() ? 1.0f : 0.5f) * (alerting ? 0.60f + 0.40f * k : 1.0f);
        auto area = getLocalBounds();

        // The beacon ring must stay INSIDE this component (anything outside its bounds is clipped):
        // ring radius max = ringMax, so the dot centre sits ringMax + 1 px from the left edge.
        const auto dotCentre = juce::Point<float> ((float) area.getX() + ringMax + 1.0f,
                                                   (float) area.getCentreY());

        if (alerting && isEnabled())
        {
            // Soft ring that grows and fades out around the dot (like a beacon).
            const float r = (float) dotSize * 0.5f + (ringMax - (float) dotSize * 0.5f) * k;   // dot radius .. ringMax
            g.setColour (dot.withAlpha (0.45f * (1.0f - k)));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (dotCentre));
        }

        g.setColour (dot.withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));
        g.fillEllipse (juce::Rectangle<float> ((float) dotSize, (float) dotSize).withCentre (dotCentre));
        area.removeFromLeft (dotSize + leftPad + 6);

        g.setColour (tint.withMultipliedAlpha (alpha));
        g.setFont (chipFont());
        g.drawText (useShort ? shortText : longText, area, juce::Justification::centredLeft, false);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (isEnabled() && onDoubleClick)
            onDoubleClick();
    }

private:
    static constexpr int   dotSize = 7;
    static constexpr float ringMax = 7.0f;                           // beacon ring radius at its widest
    static constexpr int   leftPad = 4;                              // room left of the dot for the ring
    static juce::Font chipFont() { return ptl::theme::font (ptl::theme::szSmall); }

    void timerCallback() override
    {
        pulse += (level == Level::danger ? 0.34f : 0.20f);          // danger pulses about twice as fast
        if (pulse > juce::MathConstants<float>::twoPi * 100.0f)
            pulse = 0.0f;

        if (isShowing())
            repaint();
    }

    float pulse = 0.0f;

    juce::String longText, shortText;
    Level level = Level::ok;
    bool useShort = false;
    bool hover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseChip)
};

//==============================================================================
// Small reminder bubble pointing at the licence chip: "Còn 3 ngày - gia hạn sớm ...".
// Shown by TitleBar (once a day, every start when less than a day is left). Never blocks the app.
class RenewReminder : public juce::Component
{
public:
    RenewReminder (const juce::String& headlineText, const juce::String& bodyText, bool urgent, std::function<void()> onRenewFn)
        : headline (headlineText), body (bodyText), isUrgent (urgent), onRenew (std::move (onRenewFn))
    {
        renewBtn.setButtonText (juce::String::fromUTF8 ("Gia hạn ngay"));
        renewBtn.setColour (juce::TextButton::buttonColourId, isUrgent ? ptl::theme::danger : ptl::theme::warning);
        renewBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        renewBtn.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        renewBtn.onClick = [this] { close (true); };
        addAndMakeVisible (renewBtn);

        laterBtn.setButtonText (juce::String::fromUTF8 ("Để sau"));
        laterBtn.setColour (juce::TextButton::buttonColourId, ptl::theme::raised);
        laterBtn.setColour (juce::TextButton::textColourOffId, ptl::theme::textDim);
        laterBtn.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        laterBtn.onClick = [this] { close (false); };
        addAndMakeVisible (laterBtn);

        setSize (320, 132);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (ptl::theme::panel);

        auto r = getLocalBounds().reduced (16, 14);
        const auto accent = isUrgent ? ptl::theme::danger : ptl::theme::warning;

        auto top = r.removeFromTop (20);
        g.setColour (accent);
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) top.getX() + 4.0f, (float) top.getCentreY() }));
        top.removeFromLeft (16);

        g.setColour (ptl::theme::text);
        g.setFont (ptl::theme::font (ptl::theme::szBody + 1.0f, true));
        g.drawText (headline, top, juce::Justification::centredLeft, true);

        r.removeFromTop (4);
        g.setColour (ptl::theme::textDim);
        g.setFont (ptl::theme::font (ptl::theme::szSmall + 0.5f));
        g.drawFittedText (body, r.removeFromTop (38), juce::Justification::topLeft, 3, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 14).removeFromBottom (32);
        laterBtn.setBounds (r.removeFromRight (84));
        r.removeFromRight (8);
        renewBtn.setBounds (r.removeFromRight (120));
    }

private:
    void close (bool renew)
    {
        auto cb = onRenew;                                     // this component is deleted by dismiss()

        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();

        if (renew && cb)
            juce::MessageManager::callAsync (cb);
    }

    juce::String headline, body;
    bool isUrgent = false;
    std::function<void()> onRenew;
    juce::TextButton renewBtn, laterBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RenewReminder)
};

class TitleBar : public juce::Component,
                 private juce::Timer
{
public:
    // Width of the left brand strip (animated "pt.Live | 033.247.3333", larger wordmark)
    static constexpr int brandWidth = 310;

    explicit TitleBar (juce::ResizableWindow& ownerWindow)
        : owner (ownerWindow)
    {
        setOpaque (true);

        for (auto* b : { &devicesButton, &fileButton, &pluginButton })
            addAndMakeVisible (b);

        // Licence status (plan + time left). Hidden only until the application calls setLicenseStatus().
        // Double-click = enter / replace the key while the app is running (trial -> real key, renewal).
        addChildComponent (licenseChip);
        licenseChip.onDoubleClick = [this] { if (onLicense) onLicense(); };

        // RESET: back to the settings the technician (KTV) saved. Shown only when the open file has them.
        addChildComponent (resetButton);
        resetButton.setTooltip (juce::String::fromUTF8 ("Đưa pt.Live về đúng cài đặt kỹ thuật viên đã lưu"));
        resetButton.onClick = [this] { if (onReset) onReset(); };

        // Animated brand on the far left (mouse-transparent: dragging the title bar still works).
        addAndMakeVisible (brandMark);

        // Click = Audio Settings, double-click = reload the driver. The single click waits one double-click interval
        // so a double-click is not swallowed by the (modal) dialog that the first click would open.
        devicesButton.onClick  = [this] { devicesClicked(); };
        devicesButton.setTooltip (juce::String::fromUTF8 ("Nhấn: Audio Settings\nNhấn đúp: reload driver"));
        fileButton.onClick     = [this] { if (onFileMenu) onFileMenu (fileButton); };
        pluginButton.onClick   = [this] { if (onPlugins)  onPlugins();  };
    }

    ~TitleBar() override { stopTimer(); }

    /** Animated brand on the far left: "pt.Live | 033.247.3333". */
    BrandMark brandMark;

    /** Plan + end date of the active license. Shows the chip and keeps "còn N ngày" up to date (once a minute).
        planLabel = "Dùng thử" / "1 tháng" ...; expiresAtSec = epoch seconds (0 = unknown, chip hidden). */
    void setLicenseStatus (bool isTrial, const juce::String& planLabel, juce::int64 expiresAtSec, const juce::String& tooltip)
    {
        licTrial   = isTrial;
        licPlan    = planLabel;
        licExpires = expiresAtSec;
        licenseChip.setTooltip (tooltip + juce::String::fromUTF8 ("\nNhấn đúp để nhập key hoặc gia hạn"));

        if (licExpires > 0) startTimer (60 * 1000);
        else                stopTimer();

        refreshLicenseChip();
    }

    /** Name of the open session (centre of the title bar); `unsaved` adds a * after it. */
    void setDocumentTitle (const juce::String& name, bool unsaved)
    {
        if (docTitle == name && docUnsaved == unsaved)
            return;

        docTitle    = name;
        docUnsaved  = unsaved;
        repaint();
    }

    /** Briefly shows the result of the driver reload on the Devices button ("Reloaded" / "Failed"), then restores it. */
    void flashDevicesResult (bool success)
    {
        devicesButton.setButtonText (success ? "Reloaded" : "Failed");

        juce::Component::SafePointer<TitleBar> safe (this);
        juce::Timer::callAfterDelay (1400, [safe]
        {
            if (safe != nullptr)
                safe->devicesButton.setButtonText ("Devices");
        });
    }

    /** Briefly shows the result on the File button ("Saved" / "Failed"), then restores it. */
    void flashSaveResult (bool success)
    {
        fileButton.setButtonText (success ? "Saved" : "Failed");

        juce::Component::SafePointer<TitleBar> safe (this);
        juce::Timer::callAfterDelay (1400, [safe]
        {
            if (safe != nullptr)
                safe->fileButton.setButtonText ("File");
        });
    }

    /** Locks / unlocks Devices, File, Plugins, Reset and the licence status (the window buttons keep working). */
    void setMenusEnabled (bool shouldBeEnabled)
    {
        for (auto* b : { &devicesButton, &fileButton, &pluginButton, &resetButton })
            b->setEnabled (shouldBeEnabled);

        licenseChip.setEnabled (shouldBeEnabled);
    }

    /** RESET is only shown when the open file holds a KTV copy to go back to. */
    void setResetVisible (bool shouldShow)
    {
        if (resetButton.isVisible() == shouldShow)
            return;

        resetButton.setVisible (shouldShow);
        resized();
        repaint();
    }

    //=== Callbacks ===========================================================
    std::function<void()> onDevices, onPlugins, onReset, onLicense;
    std::function<bool()> onDevicesReload;               // double-click on Devices; return true = driver reloaded
    std::function<bool()> onMaximizeRequest;             // return true = handled (collapsed panel: open the full app instead)
    std::function<void (juce::Component&)> onFileMenu;   // File button clicked; the component = menu anchor

    //=== Sizes (base px @ 100% scale) =======================================
    static constexpr int defaultHeight = ptl::theme::titleBarH;   // 42
    static constexpr int buttonWidth   = 46;
    static constexpr int buttonHeight  = 40;
    static constexpr int buttonsCount  = 3;

    //=== Layout ==============================================================
    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        // Brand area on the far left (animated BrandMark child draws on top of this).
        // Drawn in the SAME colour as the rest of the title bar (theme::panel) so it does not
        // look like a separate, lighter strip next to the chrome.
        const auto brandArea = bounds.removeFromLeft (brandWidth);
        g.setColour (ptl::theme::panel);
        g.fillRect (brandArea);

        // NOTE: the brand itself is drawn by brandMark (child component). It must NOT also be
        // drawn here, otherwise two copies overlap each other.

        g.setColour (ptl::theme::panel);
        g.fillRect (bounds);

        g.setColour (ptl::theme::hairline);
        g.fillRect (bounds.removeFromBottom (1));

        // Open session: file name centred in the free space between the brand and the buttons.
        int leftEdge = pluginButton.getX();
        for (auto* c : std::initializer_list<juce::Component*> { &licenseChip, &resetButton })
            if (c->isVisible())
                leftEdge = juce::jmin (leftEdge, c->getX());

        auto titleArea = getLocalBounds().withTrimmedLeft (brandWidth + 16)
                                         .withRight (leftEdge - 16);

        if (docTitle.isNotEmpty() && titleArea.getWidth() > 80)
        {
            g.setColour (docUnsaved ? ptl::theme::text : ptl::theme::textDim);
            g.setFont (ptl::theme::font (ptl::theme::szBody));
            g.drawText (docTitle + (docUnsaved ? " *" : ""), titleArea, juce::Justification::centred, true);
        }

        for (int i = 0; i < buttonsCount; ++i)
            drawButton (g, i, getButtonBounds (i));
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        // Brand area fixed width on the left
        brandMark.setBounds(bounds.removeFromLeft(brandWidth).withTrimmedRight(4));   // starts at x = 0: the wave runs to the app's left edge

        const int h = getHeight() - 8;
        const int y = 4;
        constexpr int gap = 2, chipGap = 6;

        // Every button is only as wide as its text (+ padding): compact, and the cluster stays clear of the brand.
        const auto widthOf = [] (const juce::String& str)
        {
            return juce::GlyphArrangement::getStringWidthInt (ptl::theme::font (ptl::theme::szBody), str);
        };

        const int tDevices = juce::jmax (widthOf ("Devices"), widthOf ("Reloaded"), widthOf ("Failed"));   // holds the reload flash too
        const int tFile    = juce::jmax (widthOf ("File"), widthOf ("Saved"), widthOf ("Failed"));   // holds the save flash too
        const int tPlugins = widthOf (pluginButton.getButtonText());
        const int tReset   = widthOf (resetButton.getButtonText());
        const bool showReset = resetButton.isVisible();
        const bool haveChip  = licExpires > 0;

        const int count   = showReset ? 4 : 3;
        const int textSum = tDevices + tFile + tPlugins + (showReset ? tReset : 0);

        int x = getButtonArea().getX() - 10;
        const int available = x - (brandWidth + 8);          // nothing may be drawn over "pt.Live | 033.247.3333"

        const int chipLong  = haveChip ? licenseChip.getIdealWidth (false) : 0;
        const int chipShort = haveChip ? licenseChip.getIdealWidth (true)  : 0;

        const auto needed = [&] (int pad, int chipW)
        {
            return textSum + count * 2 * pad + (count - 1) * gap + (chipW > 0 ? chipW + chipGap + gap : 0);
        };

        // The licence status is never dropped. Order of giving way: button padding 10 -> 6 px (full text), then the
        // short text ("còn 5 ngày") with padding 10 -> 4 px. The session name in the middle takes what is left.
        int  pad = 4;
        bool useShort = haveChip;
        bool found = false;

        for (int p = 10; p >= 6 && ! found; --p)
            if (needed (p, chipLong) <= available) { pad = p; useShort = false; found = true; }

        for (int p = 10; p >= 4 && ! found; --p)
            if (needed (p, chipShort) <= available) { pad = p; useShort = haveChip; found = true; }

        auto place = [&] (juce::Component& c, int textW)
        {
            const int w = textW + 2 * pad;
            c.setBounds (x - w, y, w, h);
            x -= w + gap;
        };

        place (fileButton,    tFile);          // File is the right-most text button
        place (devicesButton, tDevices);
        place (pluginButton,  tPlugins);

        licenseChip.setShortForm (useShort);
        licenseChip.setVisible (haveChip);

        // Order, left to right:  [licence chip] [Reset] [Plugins] [Devices] [File]
        // (placement runs right to left, so Reset is placed first and the chip after it).
        if (showReset)
            place (resetButton, tReset);

        if (haveChip)
        {
            const int w = useShort ? chipShort : chipLong;
            x -= chipGap;
            licenseChip.setBounds (x - w, y, w, h);
            x -= w + gap;
        }
    }

    //=== Mouse ===============================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        updateHoveredButton (e.getPosition());
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoveredButton != -1)
        {
            hoveredButton = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int button = getButtonAt (e.getPosition());
        if (button >= 0)
            return; // handled in mouseUp

        // Otherwise: drag the window by the title bar
        // Use the ResizableWindow as target, not the TitleBar component
        dragger.startDraggingComponent (&owner, e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (getButtonAt (e.getMouseDownPosition()) >= 0)
            return;

        // Drag the ResizableWindow, not this component
        dragger.dragComponent (&owner, e, nullptr);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const int button = getButtonAt (e.getMouseDownPosition());
        if (button >= 0)
        {
            switch (button)
            {
                case 0: owner.setMinimised (true);                   break; // minimize
                case 1: toggleMaximize();                            break; // maximize
                case 2:                                                       // close
                    if (auto* app = juce::JUCEApplicationBase::getInstance())
                        app->systemRequestedQuit();
                    break;
                default: break;
            }
            return;
        }

        // Snap-to-edge only after a real drag that ENDS at the very edge of the screen
        // (like Windows). Previously a plain click, or dropping the window anywhere in the left /
        // right third of the screen, forced it to half-screen - the user could not size it freely.
        if (! e.mouseWasDraggedSinceMouseDown() || owner.isFullScreen())
            return;

        const auto pos = e.getScreenPosition();
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForPoint (pos);
        if (display == nullptr)
            return;

        auto screenBounds = display->userArea;            // the monitor the cursor is on
        constexpr int edge = 6;

        // Snap to top => maximize
        if (pos.y <= screenBounds.getY() + edge)
        {
            if (onMaximizeRequest && onMaximizeRequest())
                return;
            owner.setFullScreen (true);
            return;
        }

        // Snap to left / right edge => half screen
        if (pos.x <= screenBounds.getX() + edge)
        {
            owner.setBounds (screenBounds.removeFromLeft (screenBounds.getWidth() / 2));
            return;
        }
        if (pos.x >= screenBounds.getRight() - edge)
        {
            owner.setBounds (screenBounds.removeFromRight (screenBounds.getWidth() / 2));
            return;
        }
    }

    // Double-click the title bar: maximise / restore (standard behaviour)
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (getButtonAt (e.getPosition()) < 0)
            toggleMaximize();
    }

    // Maximise button / double-click: while pt.Live is collapsed to the control panel this opens the full app
    // (via onMaximizeRequest) instead of making the panel full screen.
    void toggleMaximize()
    {
        if (onMaximizeRequest && onMaximizeRequest())
            return;
        owner.setFullScreen (! owner.isFullScreen());
    }

private:
    juce::ResizableWindow& owner;
    juce::ComponentDragger dragger;
    int hoveredButton = -1;

    TitleTextButton devicesButton   { "Devices" };
    TitleTextButton fileButton      { "File" };
    juce::String docTitle { "Untitled" };
    bool docUnsaved = false;
    TitleTextButton pluginButton    { "Plugins" };
    TitleTextButton resetButton     { "Reset" };

    // Devices: single click (delayed one double-click interval) vs double click.
    bool devicesClickPending = false;
    juce::uint32 devicesFirstClickAt = 0;
    int devicesClickGen = 0;

    void devicesClicked()
    {
        const auto timeout = (juce::uint32) juce::jmax (150, juce::MouseEvent::getDoubleClickTimeout());
        const auto now = juce::Time::getMillisecondCounter();

        if (devicesClickPending && now - devicesFirstClickAt <= timeout)
        {
            devicesClickPending = false;               // cancels the delayed single click
            ++devicesClickGen;

            if (onDevicesReload)
                flashDevicesResult (onDevicesReload());

            return;
        }

        devicesClickPending = true;
        devicesFirstClickAt = now;
        const int gen = ++devicesClickGen;

        juce::Component::SafePointer<TitleBar> safe (this);
        juce::Timer::callAfterDelay ((int) timeout + 40, [safe, gen]
        {
            if (safe == nullptr || ! safe->devicesClickPending || safe->devicesClickGen != gen)
                return;

            safe->devicesClickPending = false;

            if (safe->onDevices)
                safe->onDevices();
        });
    }

    LicenseChip licenseChip;
    bool licTrial = false;
    juce::String licPlan;
    juce::int64 licExpires = 0;                        // epoch seconds, 0 = unknown

    void timerCallback() override { refreshLicenseChip(); }

    // Text + colour of the chip from the stored end date: "Dùng thử · còn 5 ngày" / "còn 6 giờ" / "đã hết hạn".
    void refreshLicenseChip()
    {
        if (licExpires <= 0)
        {
            resized();
            return;
        }

        constexpr juce::int64 hour = 3600, day = 24 * hour;
        const juce::int64 left = licExpires - juce::Time::currentTimeMillis() / 1000;

        juce::String remain;
        if      (left <= 0)    remain = juce::String::fromUTF8 ("đã hết hạn");
        else if (left >= day)  remain = juce::String::fromUTF8 ("còn ") + juce::String ((left + day - 1) / day)   + juce::String::fromUTF8 (" ngày");
        else if (left >= hour) remain = juce::String::fromUTF8 ("còn ") + juce::String ((left + hour - 1) / hour) + juce::String::fromUTF8 (" giờ");
        else                   remain = juce::String::fromUTF8 ("còn dưới 1 giờ");

        // Trial is short, so it warns earlier relative to its length than a paid plan warns.
        const juce::int64 warnWithin = (licTrial ? 3 : 7) * day;

        auto level = licTrial ? LicenseChip::Level::trial : LicenseChip::Level::ok;
        if      (left <= day)        level = LicenseChip::Level::danger;
        else if (left <= warnWithin) level = LicenseChip::Level::warn;

        const auto plan = licTrial ? licPlan : juce::String::fromUTF8 ("Gói ") + licPlan;
        licenseChip.setContent (plan + juce::String::fromUTF8 (" · ") + remain, remain, level);

        resized();                                     // the chip width follows the text
        repaint();

        if (left > 0 && left <= warnWithin)
            maybeRemindToRenew (left);
    }

    //=== Renewal reminder ====================================================
    // Close to the end of the licence the chip pulses (LicenseChip) AND a small bubble points at it:
    // once per calendar day, and at every start when less than one day is left. "Để sau" just closes it.
    bool reminderHandled = false;

    static juce::File reminderStampFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ptLive").getChildFile ("renew_reminder.txt");
    }

    void maybeRemindToRenew (juce::int64 leftSec)
    {
        if (reminderHandled)
            return;

        reminderHandled = true;                        // at most once per run

        constexpr juce::int64 day = 24 * 3600;
        const auto key = juce::Time::getCurrentTime().formatted ("%Y-%m-%d") + "|" + juce::String (licExpires);
        const auto stamp = reminderStampFile();

        if (leftSec > day && stamp.loadFileAsString().trim() == key)
            return;                                    // already reminded today

        stamp.getParentDirectory().createDirectory();
        stamp.replaceWithText (key);

        juce::Component::SafePointer<TitleBar> safe (this);
        juce::Timer::callAfterDelay (2500, [safe] { if (safe != nullptr) safe->showRenewReminder (0); });
    }

    void showRenewReminder (int attempt)
    {
        if (! isShowing() || ! licenseChip.isShowing())
        {
            if (attempt < 10)                          // window still starting up: try again shortly
            {
                juce::Component::SafePointer<TitleBar> safe (this);
                juce::Timer::callAfterDelay (1500, [safe, attempt] { if (safe != nullptr) safe->showRenewReminder (attempt + 1); });
            }
            return;
        }

        if (! licenseChip.isEnabled())                 // UI locked during start-up
        {
            if (attempt < 10)
            {
                juce::Component::SafePointer<TitleBar> safe (this);
                juce::Timer::callAfterDelay (1500, [safe, attempt] { if (safe != nullptr) safe->showRenewReminder (attempt + 1); });
            }
            return;
        }

        constexpr juce::int64 hour = 3600, day = 24 * hour;
        const juce::int64 left = licExpires - juce::Time::currentTimeMillis() / 1000;

        if (left <= 0)
            return;

        const bool urgent = left <= day;
        juce::String remain;
        if (left >= day) remain = juce::String ((left + day - 1) / day) + juce::String::fromUTF8 (" ngày");
        else             remain = juce::String::fromUTF8 ("chưa đầy ") + juce::String ((left + hour - 1) / hour) + juce::String::fromUTF8 (" giờ");

        const auto headline = urgent ? juce::String::fromUTF8 ("Bản quyền sắp hết hạn!")
                                     : juce::String::fromUTF8 ("Sắp đến hạn gia hạn");
        const auto body = juce::String::fromUTF8 ("Bản quyền pt.Live còn ") + remain
                        + juce::String::fromUTF8 (". Gia hạn sớm để không bị gián đoạn khi đang hát.");

        juce::Component::SafePointer<TitleBar> safeBar (this);
        auto content = std::make_unique<RenewReminder> (headline, body, urgent,
                                                        [safeBar] { if (safeBar != nullptr && safeBar->onLicense) safeBar->onLicense(); });

        juce::CallOutBox::launchAsynchronously (std::move (content), licenseChip.getScreenBounds(), nullptr);
    }

    juce::Rectangle<int> getButtonArea() const
    {
        return getLocalBounds().removeFromRight (buttonWidth * buttonsCount);
    }

    juce::Rectangle<int> getButtonBounds (int index) const
    {
        auto area = getButtonArea();
        const int w = area.getWidth() / buttonsCount;
        return area.withX (area.getX() + index * w).withWidth (w);
    }

    int getButtonAt (juce::Point<int> pos) const
    {
        for (int i = 0; i < buttonsCount; ++i)
            if (getButtonBounds (i).contains (pos))
                return i;
        return -1;
    }

    void updateHoveredButton (juce::Point<int> pos)
    {
        const int b = getButtonAt (pos);
        if (b != hoveredButton)
        {
            hoveredButton = b;
            repaint();
        }
    }

    void drawButton (juce::Graphics& g, int index, juce::Rectangle<int> bounds)
    {
        using namespace ptl::theme;

        const bool isClose = (index == 2);
        const bool isHover = (index == hoveredButton);

        if (isHover && isClose)
        {
            g.setColour (danger);
            g.fillRect (bounds);
        }
        else if (isHover)
        {
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.fillRect (bounds);
        }

        g.setColour (isHover && isClose ? textOnAccent : isHover ? text : textDim);

        const auto c = bounds.getCentre().toFloat();

        switch (index)
        {
            case 0:     // minimise: 10px line
                g.drawLine (c.x - 5.0f, c.y, c.x + 5.0f, c.y, 1.0f);
                break;

            case 1:     // maximise: 9.5px rounded square
                g.drawRoundedRectangle (juce::Rectangle<float> (9.5f, 9.5f).withCentre (c), 1.5f, 1.0f);
                break;

            case 2:     // close: X
            {
                const float s = 4.5f;
                g.drawLine (c.x - s, c.y - s, c.x + s, c.y + s, 1.1f);
                g.drawLine (c.x + s, c.y - s, c.x - s, c.y + s, 1.1f);
                break;
            }
            default: break;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TitleBar)
};
