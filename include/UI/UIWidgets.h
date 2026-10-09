/*
    pt.Live - Live Singing DAW
    Small shared UI widgets: IconButton (SVG glyph, state tinted), VuStrip (per-track segmented meter),
    MasterMeter (vertical LED meter).  Header-only.
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include "include/Theme.h"
#include "include/Icons.h"

namespace ptl::ui
{
    // "AUDIO INPUT" -> "A U D I O   I N P U T"  (kept for dialogs that still use it)
    inline juce::String spaced (const juce::String& s)
    {
        juce::String out;
        for (int i = 0; i < s.length(); ++i)
        {
            out += s.substring (i, i + 1);
            if (i < s.length() - 1)
                out += " ";
        }
        return out;
    }

    // dB -> 0..1 meter fraction: IEC 60268-18 deflection (the scale used by pro DAW meters).
    // It is NOT linear in dB: the quiet region is compressed so a whisper / room noise stays low and
    // the bar only really moves for a real singing level.
    //     -70 dB ->  0 %     -50 dB ->  7.5 %    -30 dB -> 30 %    -10 dB -> 75 %
    //     -60 dB ->  2.5 %   -40 dB -> 15 %      -20 dB -> 50 %      0 dB -> 100 %
    // (The old linear -72..0 dB mapping put -36 dB, i.e. a very quiet sound, at 50 %.)
    inline float dbToFraction (float db)
    {
        float pct;

        if      (db <= -60.0f) pct = 0.0f;      // noise floor / silence: no LED at all
        else if (db < -60.0f) pct = (db + 70.0f) * 0.25f;
        else if (db < -50.0f) pct = (db + 60.0f) * 0.50f + 2.5f;
        else if (db < -40.0f) pct = (db + 50.0f) * 0.75f + 7.5f;
        else if (db < -30.0f) pct = (db + 40.0f) * 1.50f + 15.0f;
        else if (db < -20.0f) pct = (db + 30.0f) * 2.00f + 30.0f;
        else                  pct = (db + 20.0f) * 2.50f + 50.0f;

        return juce::jlimit (0.0f, 1.0f, pct / 100.0f);
    }

    //==========================================================================
    // Flat, borderless icon chip. Glyph = 0.65 x box, tinted by state (spec 16.4).
    class IconButton : public juce::Button
    {
    public:
        enum class Style { Neutral, Mute, Solo, Delete };

        explicit IconButton (icons::Id i, Style s = Style::Neutral)
            : juce::Button (juce::String()), icon (i), style (s) {}

        void setFillColour (juce::Colour c) { fill = c; repaint(); }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            using namespace ptl::theme;

            const bool en = isEnabled();
            const bool on = getToggleState();

            auto bg  = fill;
            auto tint = textDim;

            if (! en)
            {
                bg   = bg.withMultipliedAlpha (0.33f);
                tint = textFaint;
            }
            else if (on && style == Style::Mute)   { bg = danger; tint = textOnAccent; }
            else if (on && style == Style::Solo)   { bg = ptl::theme::text;   tint = soloText; }
            else if (over && style == Style::Delete) { bg = danger.withAlpha (0.22f); tint = danger; }
            else if (over)                         { bg = bg.brighter (0.06f); tint = ptl::theme::text; }

            if (en && down && ! on)
                bg = bg.brighter (0.12f);

            auto b = getLocalBounds().toFloat();
            g.setColour (bg);
            g.fillRoundedRectangle (b, juce::jmin (10.0f, b.getHeight() * 0.25f));

            const float side = juce::jmin (b.getWidth(), b.getHeight()) * 0.65f;
            icons::draw (g, icon, b.withSizeKeepingCentre (side, side), tint);
        }

    private:
        icons::Id icon;
        Style style;
        juce::Colour fill = ptl::theme::raised;
    };

    //==========================================================================
    // Per-track meter: same look as the master meter, laid on its side.
    // Two rows (L on top, R below, 1px gap) built from many small LED segments
    // (2px wide, 1px gap), same colour zones and 1.5 s peak-hold as MasterMeter.
    class VuStrip : public juce::Component
    {
    public:
        VuStrip() { setInterceptsMouseClicks (false, false); }

        void setLevels (float leftDb, float rightDb)
        {
            const auto before = snapshot();

            update (0, dbToFraction (leftDb));
            update (1, dbToFraction (rightDb));

            if (snapshot() != before)                  // repaint only when a segment actually changes
                repaint();
        }

        void paint (juce::Graphics& g) override
        {
            using namespace ptl::theme;

            const int numSeg = numSegments();
            const int rowH   = juce::jmax (1, (getHeight() - kRowGap) / 2);

            for (int ch = 0; ch < 2; ++ch)
            {
                const int y        = (ch == 0) ? 0 : rowH + kRowGap;
                const int litSegs  = juce::roundToInt (shown[ch] * (float) numSeg);
                const int peakSeg  = juce::jlimit (0, numSeg - 1, juce::roundToInt (peak[ch] * (float) numSeg) - 1);

                for (int i = 0; i < numSeg; ++i)
                {
                    const float pos  = ((float) i + 0.5f) / (float) numSeg;
                    const auto band  = pos < 0.60f ? meterLow : pos < 0.85f ? meterMid : meterHigh;
                    const int x      = i * (kSegW + kSegGap);

                    g.setColour (i < litSegs ? band : band.withAlpha (0.10f));
                    g.fillRect (x, y, kSegW, rowH);

                    if (peak[ch] > 0.02f && i == peakSeg && peakSeg >= litSegs)
                    {
                        g.setColour (text.withAlpha (0.85f));
                        g.fillRect (x, y, kSegW, rowH);
                    }
                }
            }
        }

    private:
        static constexpr int kSegW = 2, kSegGap = 1, kRowGap = 1;
        static constexpr int kPeakHoldFrames = 45;                    // 1.5 s @ 30 Hz
        static constexpr float kPeakFallStep = 0.012f;

        int numSegments() const { return juce::jmax (1, (getWidth() + kSegGap) / (kSegW + kSegGap)); }

        // Everything that is visible: lit count + peak segment of both channels.
        std::array<int, 4> snapshot() const
        {
            const int n = numSegments();
            return { juce::roundToInt (shown[0] * (float) n), juce::roundToInt (peak[0] * (float) n),
                     juce::roundToInt (shown[1] * (float) n), juce::roundToInt (peak[1] * (float) n) };
        }

        void update (int ch, float frac)
        {
            shown[ch] = frac > shown[ch] ? frac : shown[ch] * 0.85f + frac * 0.15f;   // instant attack, ~150 ms release

            if (frac >= peak[ch])
            {
                peak[ch] = frac;
                hold[ch] = kPeakHoldFrames;
            }
            else if (hold[ch] > 0)
                --hold[ch];
            else
                peak[ch] = juce::jmax (0.0f, peak[ch] - kPeakFallStep);
        }

        float shown[2] { 0.0f, 0.0f }, peak[2] { 0.0f, 0.0f };
        int hold[2] { 0, 0 };
    };

    //==========================================================================
    // Master meter: two LED columns separated by a 1px gap, no border / well, peak-hold 1.5 s.
    // setShowScale(true) (the main window's strip): wider LED columns + a dB scale (0 / -6 / -12 / -24 / -48) beside them,
    // so the 44px column is not a thin line in empty space. The collapsed control bar keeps the plain meter.
    class MasterMeter : public juce::Component
    {
    public:
        MasterMeter() { setInterceptsMouseClicks (false, false); }

        void setShowScale (bool on) { if (showScale != on) { showScale = on; repaint(); } }

        void setLevels (float leftDb, float rightDb)
        {
            update (0, dbToFraction (leftDb));
            update (1, dbToFraction (rightDb));
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            using namespace ptl::theme;

            const int segW = showScale ? kSegWScale : kSegW;
            const int scaleW = showScale ? kScaleW : 0;

            const int numSeg = juce::jmax (1, (getHeight() + kSegGap) / (kSegH + kSegGap));
            // [scale] + L + 1px gap + R, centred in the component.
            const int pairW  = 2 * segW + kChannelGap;
            const int totalW = scaleW + pairW;
            const int startX = (getWidth() - totalW) / 2;
            const int pairX  = startX + scaleW;

            if (showScale)
            {
                // dB scale: tick + label at the height where the LEDs for that level end.
                g.setFont (font (9.0f, true));
                const int usedH = numSeg * (kSegH + kSegGap) - kSegGap;
                const int topY  = getHeight() - usedH;     // numSeg segments, bottom aligned (see below)
                for (const int db : { 0, -6, -12, -24, -48 })
                {
                    const float frac = dbToFraction ((float) db);
                    const int   y    = getHeight() - juce::roundToInt (frac * (float) numSeg) * (kSegH + kSegGap) + (db == 0 ? 1 : 0);
                    const int   cy   = juce::jlimit (topY + 5, getHeight() - 5, y);
                    g.setColour (db >= -6 ? (db == 0 ? meterHigh : meterMid).withAlpha (0.95f) : textMid);
                    g.drawText (juce::String (db), startX - 2, cy - 6, scaleW - 3, 12, juce::Justification::centredRight, false);
                    g.setColour (outlineStrong);
                    g.fillRect (startX + scaleW - 2, cy, 2, 1);
                }
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                const int x = (ch == 0) ? pairX : pairX + segW + kChannelGap;
                const int litSegs  = juce::roundToInt (shown[ch] * (float) numSeg);
                const int peakSeg  = juce::jlimit (0, numSeg - 1, juce::roundToInt (peak[ch] * (float) numSeg) - 1);

                for (int i = 0; i < numSeg; ++i)
                {
                    const float pos = ((float) i + 0.5f) / (float) numSeg;
                    const auto band = pos < 0.60f ? meterLow : pos < 0.85f ? meterMid : meterHigh;
                    const int y = getHeight() - (i + 1) * (kSegH + kSegGap) + kSegGap;

                    const bool lit = i < litSegs;
                    g.setColour (lit ? band : band.withAlpha (showScale ? 0.16f : 0.10f));
                    g.fillRect (x, y, segW, kSegH);

                    if (peak[ch] > 0.02f && i == peakSeg && peakSeg >= litSegs)
                    {
                        g.setColour (text.withAlpha (0.85f));
                        g.fillRect (x, y, segW, kSegH);
                    }
                }
            }
        }

    private:
        static constexpr int kSegW = 6, kSegH = 2, kSegGap = 1;
        static constexpr int kSegWScale = 9;       // LED width when the dB scale is shown
        static constexpr int kScaleW = 20;         // width of the dB scale column
        static constexpr int kChannelGap = 1;     // gap between the Left and Right columns
        static constexpr int kPeakHoldFrames = 45;
        static constexpr float kPeakFallStep = 0.012f;

        void update (int ch, float frac)
        {
            shown[ch] = frac > shown[ch] ? frac : shown[ch] * 0.85f + frac * 0.15f;

            if (frac >= peak[ch])
            {
                peak[ch] = frac;
                hold[ch] = kPeakHoldFrames;
            }
            else if (hold[ch] > 0)
                --hold[ch];
            else
                peak[ch] = juce::jmax (0.0f, peak[ch] - kPeakFallStep);
        }

        bool showScale = false;
        float shown[2] { 0.0f, 0.0f }, peak[2] { 0.0f, 0.0f };
        int hold[2] { 0, 0 };
    };
}
