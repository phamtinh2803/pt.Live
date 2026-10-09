/*
    pt.Live - Live Singing DAW
    Central theme: palette, metrics and fonts  (see UI_Specification - Complete.md)

    Near-monochrome UI: hue is reserved for STATE (mute, solo, record, clip, active value).
    Single font family: IBM Plex Mono.
      - If assets/fonts/IBMPlexMono-Regular.ttf + IBMPlexMono-Bold.ttf exist, CMake embeds them
        (PTLIVE_HAS_PLEX=1).
      - Otherwise the font is looked up by name, then falls back to Consolas / Courier New.
*/

#pragma once

#include <JuceHeader.h>

#if PTLIVE_HAS_PLEX
 #include <BinaryData.h>
#endif

namespace ptl::theme
{
    //== Surfaces (dark -> light) =============================================
    inline const juce::Colour window   { 0xff131316 };
    inline const juce::Colour panel    { 0xff18181c };
    inline const juce::Colour raised   { 0xff202024 };
    inline const juce::Colour raisedHi { 0xff28282e };
    inline const juce::Colour stage    { 0xff0e0e11 };

    //== Lines ================================================================
    inline const juce::Colour hairline      { 0x12ffffff };
    inline const juce::Colour outline       { 0x22ffffff };
    inline const juce::Colour outlineStrong { 0x38ffffff };

    //== Text =================================================================
    inline const juce::Colour text         { 0xfff2f2f4 };
    inline const juce::Colour textDim      { 0xff9a9aa4 };
    inline const juce::Colour textFaint    { 0xff63636e };
    inline const juce::Colour textMid      { 0xffc6c6ce };   // between text and textDim: captions / labels that must stay readable at a glance
    inline const juce::Colour textOnAccent { 0xffffffff };

    //== Accents (state only) =================================================
    inline const juce::Colour accent       { 0xff569cd6 };
    inline const juce::Colour accentHover  { 0xff6fb0e6 };
    inline const juce::Colour accentDeep   { 0xff3a6fa5 };
    inline const juce::Colour danger       { 0xffc0504a };
    inline const juce::Colour dangerHover  { 0xffd2605a };
    inline const juce::Colour warning      { 0xffd8a13c };
    inline const juce::Colour warningHover { 0xffe6b04e };
    inline const juce::Colour success      { 0xff5a9e6f };
    inline const juce::Colour solo         { 0xffd8a13c };
    inline const juce::Colour songTitleCol { 0xff6fe3d3 };   // title of the song being sung (control bar strip): its own hue, never used for a state
    inline const juce::Colour recordRed    { 0xffe0483f };   // RECORD button (idle ring / recording fill)
    inline const juce::Colour soloText     { 0xff17171a };

    // Heavy plugin indicator state
    inline const juce::Colour heavyPlugin  = juce::Colour (0xffc0504a).withAlpha (0.25f);
    inline const juce::Colour heavyPluginBorder { 0xffc0504a };
    // Insert whose VST is not in the scanned plugin list (missing / not scanned yet)
    inline const juce::Colour missingPluginBorder { 0xffe05048 };

    //== Selection / rows =====================================================
    inline const juce::Colour selection      { 0xff34343c };
    inline const juce::Colour selectionHover { 0xff3b3b45 };
    inline const juce::Colour rowIdle        { 0xff24242a };
    inline const juce::Colour rowHover       { 0xff2c2c33 };

    //== Metering =============================================================
    inline const juce::Colour meterLow   { 0xff5a9e6f };
    inline const juce::Colour meterMid   { 0xffd8a13c };
    inline const juce::Colour meterHigh  { 0xffd2605a };
    inline const juce::Colour meterTrack { 0xff0a0a0c };

    //== Lanes (structure, not hue) ===========================================
    inline const juce::Colour laneFill  = juce::Colours::white.withAlpha (0.022f);
    inline const juce::Colour laneAudio = juce::Colours::white.withAlpha (1.00f);
    inline const juce::Colour laneFx    = juce::Colours::white.withAlpha (0.75f);
    inline const juce::Colour laneOut   = juce::Colours::white.withAlpha (1.00f);
    inline const juce::Colour sectionRail  = juce::Colours::white.withAlpha (0.70f);
    inline const juce::Colour sectionLabel { 0xffd8d8de };

    //== Legacy aliases (dialogs written against the old theme keep compiling) =
    inline const juce::Colour bgPrimary    = window;
    inline const juce::Colour bgSecondary  = panel;
    inline const juce::Colour bgTertiary   = raised;
    inline const juce::Colour bgGroup      = stage;
    inline const juce::Colour textPrimary   = text;
    inline const juce::Colour textSecondary = textDim;
    inline const juce::Colour textDimmed    = textFaint;
    inline const juce::Colour accentBlue    = accent;
    inline const juce::Colour accentGreen   = success;
    inline const juce::Colour accentRed     = danger;
    inline const juce::Colour borderLight   = outline;
    inline const juce::Colour borderDark    = hairline;
    inline const juce::Colour vuGreen  = meterLow;
    inline const juce::Colour vuYellow = meterMid;
    inline const juce::Colour vuOrange = meterMid;
    inline const juce::Colour vuRed    = meterHigh;

    //== Text sizes (JUCE font heights) =======================================
    constexpr float szMicro = 11.5f;
    constexpr float szSmall = 12.0f;
    constexpr float szBody  = 13.5f;
    constexpr float szLabel = 15.0f;
    constexpr float szTitle = 17.0f;

    //== Metrics ==============================================================
    constexpr float radius   = 4.0f;
    constexpr float radiusLg = 6.0f;

    constexpr int titleBarH  = 42;
    constexpr int bottomBarH = 40;
    constexpr int controlBarH = 120;      // live control panel (30 title strip + 60 buttons + 30 sliders), above the bottom bar
    constexpr int inspectorW = 300;
    constexpr int masterMeterW = 52;     // right strip: dB scale + wide LEDs (see ptl::ui::MasterMeter::setShowScale)

    constexpr int baseWindowWidth  = 1080;
    constexpr int baseWindowHeight = 750;
    constexpr int minWindowWidth   = 800;
    constexpr int minWindowHeight  = 500;

    // legacy
    constexpr int headerHeight    = titleBarH;
    constexpr int transportHeight = 0;
    constexpr int footerHeight    = bottomBarH;

    //== Fonts ================================================================
    namespace detail
    {
        struct FontFace
        {
            juce::String name;
            float scale = 1.0f;     // fallback fonts are taller per "height" than Plex Mono
           #if PTLIVE_HAS_PLEX
            juce::Typeface::Ptr regular, bold;
           #endif

            FontFace()
            {
               #if PTLIVE_HAS_PLEX
                regular = juce::Typeface::createSystemTypefaceFor (BinaryData::IBMPlexMono_Regular_ttf,
                                                                   BinaryData::IBMPlexMono_Regular_ttfSize);
                bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::IBMPlexMono_Bold_ttf,
                                                                   BinaryData::IBMPlexMono_Bold_ttfSize);
                if (regular != nullptr && bold != nullptr)
                    return;
               #endif

                const auto installed = juce::Font::findAllTypefaceNames();

                for (const char* candidate : { "IBM Plex Mono", "Consolas", "Cascadia Mono", "Courier New" })
                {
                    if (installed.contains (candidate))
                    {
                        name = candidate;
                        break;
                    }
                }

                if (name.isEmpty())
                    name = juce::Font::getDefaultMonospacedFontName();

                scale = (name == "IBM Plex Mono") ? 1.0f : 0.86f;
            }
        };

        inline const FontFace& face()
        {
            static const FontFace f;
            return f;
        }
    }

    inline juce::Font font (float height, bool bold = false)
    {
        const auto& f = detail::face();

       #if PTLIVE_HAS_PLEX
        if (f.regular != nullptr && f.bold != nullptr)
            return juce::Font (juce::FontOptions (bold ? f.bold : f.regular).withHeight (height));
       #endif

        return juce::Font (juce::FontOptions (f.name, height * f.scale,
                                              bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::Font monoFont (float height) { return font (height, false); }

    // Letter-spaced heading font. `tracking` is JUCE's extra-kerning factor (fraction of the height).
    inline juce::Font spacedFont (float height, float tracking = 0.9f, bool bold = true)
    {
        return font (height, bold).withExtraKerningFactor (tracking);
    }

    // Launches a DialogWindow whose title bar has the same height as the main app's title bar.
    // (Colours / buttons come from PtLiveLookAndFeel.) Returns the window, or nullptr.
    inline juce::DialogWindow* launchDialog (juce::DialogWindow::LaunchOptions& opts,
                                             int minW = 0, int minH = 0, int maxW = 4000, int maxH = 4000)
    {
        auto* w = opts.create();

        if (w == nullptr)
            return nullptr;

        w->setTitleBarHeight (titleBarH);

        if (opts.resizable && minW > 0 && minH > 0)
            w->setResizeLimits (minW, minH, maxW, maxH);

        w->enterModalState (true, nullptr, true);
        return w;
    }
}
