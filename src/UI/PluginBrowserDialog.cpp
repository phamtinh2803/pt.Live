/*
    pt.Live - Live Singing DAW
    PluginBrowserDialog implementation

    Table: Name | Vendor | Category | VST | Version | Architecture
*/

#include "include/UI/PluginBrowserDialog.h"
#include "include/Theme.h"

using namespace ptl;

namespace
{
    constexpr int rowH      = 32;
    constexpr int headerH   = 34;
    constexpr int toolbarH  = 38;
    constexpr int statusH   = 84;
    constexpr int bottomH   = 36;

    constexpr float szRow   = 14.0f;     // table text
    constexpr float szCell2 = 13.5f;     // secondary columns

    //== Category filter ======================================================
    struct CatDef { const char* label; std::vector<const char*> keys; };   // vector: an initializer_list member would dangle

    // ids: 1 All, 2 Instruments, 3 Effects, 4.. = entries below, last = Other
    const std::vector<CatDef>& categoryDefs()
    {
        static const std::vector<CatDef> defs
        {
            { "EQ",          { "EQ", "Equal" } },
            { "Dynamics",    { "Dynamics", "Compressor", "Limiter", "Gate", "Expander", "De-Ess" } },
            { "Reverb",      { "Reverb" } },
            { "Delay",       { "Delay", "Echo" } },
            { "Distortion",  { "Distortion", "Saturat", "Amp" } },
            { "Modulation",  { "Modulation", "Chorus", "Flanger", "Phaser" } },
            { "Filter",      { "Filter" } },
            { "Pitch Shift", { "Pitch" } },
            { "Analyzer",    { "Analyzer", "Analysis", "Meter" } },
        };
        return defs;
    }

    bool matchesDef (const juce::PluginDescription& d, const CatDef& def)
    {
        for (auto* k : def.keys)
            if (d.category.containsIgnoreCase (k))
                return true;
        return false;
    }

    bool matchesCategory (const juce::PluginDescription& d, int id)
    {
        const int firstDef = 4;
        const int otherId  = firstDef + (int) categoryDefs().size();

        if (id <= 1)  return true;
        if (id == 2)  return d.isInstrument;
        if (id == 3)  return ! d.isInstrument;

        if (id == otherId)
        {
            for (auto& def : categoryDefs())
                if (matchesDef (d, def))
                    return false;
            return true;
        }

        const int idx = id - firstDef;
        return idx >= 0 && idx < (int) categoryDefs().size() && matchesDef (d, categoryDefs()[(size_t) idx]);
    }

    juce::String prettyCategory (const juce::PluginDescription& d)
    {
        auto c = d.category.trim();
        if (c.isEmpty())
            return d.isInstrument ? "Instrument" : "Fx";
        return c.replace ("|", " / ");
    }

    juce::String vstLabel (const juce::PluginDescription& d)
    {
        if (d.pluginFormatName.containsIgnoreCase ("VST3")) return "VST3";
        if (d.pluginFormatName.containsIgnoreCase ("VST"))  return "VST2";
        return d.pluginFormatName;
    }

    //== Architecture (read from the file, JUCE does not store it) ============
    juce::String archFromPE (const juce::File& f)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk() || in.getTotalLength() < 0x100)
            return {};

        if ((juce::uint16) in.readShort() != 0x5A4D)               // "MZ"
            return {};

        in.setPosition (0x3C);
        const auto peOffset = (juce::int64) (juce::uint32) in.readInt();
        in.setPosition (peOffset);

        if ((juce::uint32) in.readInt() != 0x00004550u)            // "PE\0\0"
            return {};

        switch ((juce::uint16) in.readShort())
        {
            case 0x014c: return "32-bit";
            case 0x8664: return "64-bit";
            case 0xAA64: return "ARM64";
            default:     return "Unknown";
        }
    }

    juce::String detectArchitecture (const juce::PluginDescription& d)
    {
        juce::File f (d.fileOrIdentifier);

        if (f.isDirectory())      // VST3 bundle: Contents/<arch>-win/Plugin.vst3
        {
            const bool x64 = f.getChildFile ("Contents/x86_64-win").isDirectory();
            const bool x86 = f.getChildFile ("Contents/x86-win").isDirectory();
            const bool arm = f.getChildFile ("Contents/arm64-win").isDirectory()
                          || f.getChildFile ("Contents/arm64ec-win").isDirectory();

            if (x64 && x86) return "32 + 64-bit";
            if (x64)        return "64-bit";
            if (x86)        return "32-bit";
            if (arm)        return "ARM64";
            return "-";
        }

        if (f.existsAsFile())
        {
            auto a = archFromPE (f);
            if (a.isNotEmpty())
                return a;
        }

        return "-";
    }

    juce::String rowKey (const juce::PluginDescription& d)
    {
        return d.fileOrIdentifier + "|" + d.name + "|" + juce::String (d.uniqueId);
    }

    void drawPill (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour textColour,
                   const juce::String& text, float fontH)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (textColour);
        g.setFont (theme::font (fontH, true));
        g.drawText (text, r.toNearestInt(), juce::Justification::centred, false);
    }
}

//==============================================================================
// Header look
//==============================================================================
void PluginBrowserDialog::HeaderLookAndFeel::drawTableHeaderBackground (juce::Graphics& g,
                                                                        juce::TableHeaderComponent& h)
{
    auto r = h.getLocalBounds();
    g.setColour (theme::raised);
    g.fillRect (r);
    g.setColour (theme::outlineStrong);
    g.fillRect (r.removeFromBottom (1));
}

void PluginBrowserDialog::HeaderLookAndFeel::drawTableHeaderColumn (juce::Graphics& g,
                                                                    juce::TableHeaderComponent& header,
                                                                    const juce::String& name, int /*columnId*/,
                                                                    int width, int height, bool over, bool down,
                                                                    int columnFlags)
{
    if (down)      g.fillAll (theme::raisedHi);
    else if (over) g.fillAll (theme::rowHover);

    g.setColour (theme::hairline);
    g.fillRect (width - 1, 8, 1, height - 16);

    auto area = juce::Rectangle<int> (0, 0, width, height).reduced (12, 0);

    const bool fwd    = (columnFlags & juce::TableHeaderComponent::sortedForwards) != 0;
    const bool bwd    = (columnFlags & juce::TableHeaderComponent::sortedBackwards) != 0;
    const bool sorted = fwd || bwd;
    juce::ignoreUnused (header);

    if (sorted)
    {
        auto arrow = area.removeFromRight (16).toFloat();
        const float cx = arrow.getCentreX(), cy = arrow.getCentreY();
        juce::Path p;
        if (fwd) p.addTriangle (cx - 4.5f, cy + 2.5f, cx + 4.5f, cy + 2.5f, cx, cy - 3.0f);
        else     p.addTriangle (cx - 4.5f, cy - 2.5f, cx + 4.5f, cy - 2.5f, cx, cy + 3.0f);
        g.setColour (theme::accent);
        g.fillPath (p);
    }

    g.setColour (sorted ? theme::text : theme::textDim);
    g.setFont (theme::font (12.5f, true).withExtraKerningFactor (0.06f));
    g.drawFittedText (name.toUpperCase(), area, juce::Justification::centredLeft, 1);
}

//==============================================================================
// Scan status panel
//==============================================================================
PluginScanStatusPanel::PluginScanStatusPanel()
{
    setInterceptsMouseClicks (false, false);
    resultColour = theme::success;
}

bool PluginScanStatusPanel::isBlinking() const noexcept
{
    return (state == State::finished || state == State::cancelled)
        && (juce::Time::getMillisecondCounterHiRes() - blinkStartMs) < blinkDurationMs;
}

void PluginScanStatusPanel::ensureTimer()
{
    if ((isBusy() || isBlinking()) && ! isTimerRunning())
        startTimerHz (30);
}

void PluginScanStatusPanel::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();

    if (isBusy())
    {
        elapsedMs = now - busyStartMs;

        const float target = (totalFiles > 0 && state == State::scanning)
                                 ? juce::jlimit (0.0f, 1.0f, (float) doneFiles / (float) totalFiles)
                                 : 0.0f;
        displayedFraction += (target - displayedFraction) * 0.25f;
    }
    else if (! isBlinking())
    {
        stopTimer();
    }

    repaint();
}

void PluginScanStatusPanel::setIdle (int total)
{
    state = State::idle;
    totalPlugins = total;
    displayedFraction = 0.0f;
    stopTimer();
    repaint();
}

void PluginScanStatusPanel::setProgress (PluginScanner::Phase phase, int done, int total, int newFoundNow,
                                         const juce::String& item, const juce::String& format)
{
    const bool wasBusy = isBusy();

    using P = PluginScanner::Phase;
    state = (phase == P::saving)   ? State::saving
          : (phase == P::scanning && total > 0) ? State::scanning
          : State::searching;

    if (! wasBusy)
    {
        busyStartMs = juce::Time::getMillisecondCounterHiRes();
        displayedFraction = 0.0f;
    }

    doneFiles = done;
    totalFiles = total;
    newFound = newFoundNow;
    currentItem = item;
    currentFormat = format;

    ensureTimer();
    repaint();
}

void PluginScanStatusPanel::showFinished (int newCount, int failed, int total, bool cancelled)
{
    state = cancelled ? State::cancelled : State::finished;
    newFound = newCount;
    failedCount = failed;
    totalPlugins = total;
    resultColour = cancelled ? theme::warning : theme::success;
    blinkStartMs = juce::Time::getMillisecondCounterHiRes();
    displayedFraction = 1.0f;

    ensureTimer();
    repaint();
}

void PluginScanStatusPanel::paint (juce::Graphics& g)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    auto b = getLocalBounds().toFloat().reduced (0.5f);

    //== blink (2 s) ==========================================================
    float blink = 0.0f;
    if (isBlinking())
    {
        const double t = (now - blinkStartMs) / 1000.0;
        blink = 0.5f * (1.0f + (float) std::cos (juce::MathConstants<double>::twoPi * 2.0 * t));   // 2 blinks / s
    }

    const bool showResult = (state == State::finished || state == State::cancelled);

    //== card ================================================================
    g.setColour (theme::raised);
    g.fillRoundedRectangle (b, theme::radiusLg);

    if (showResult)
    {
        g.setColour (resultColour.withAlpha (0.10f + 0.28f * blink));
        g.fillRoundedRectangle (b, theme::radiusLg);
    }

    g.setColour (showResult ? resultColour.withAlpha (0.55f + 0.45f * blink)
                            : (isBusy() ? theme::accent.withAlpha (0.55f) : theme::outline));
    g.drawRoundedRectangle (b, theme::radiusLg, showResult ? 1.0f + 1.5f * blink : 1.0f);

    auto inner = getLocalBounds().reduced (16, 12);

    //== icon ================================================================
    auto iconArea = inner.removeFromLeft (44).withSizeKeepingCentre (40, 40).translated (0, -6).toFloat();
    const float cx = iconArea.getCentreX(), cy = iconArea.getCentreY(), rad = 17.0f;

    if (isBusy())
    {
        g.setColour (theme::outline);
        g.drawEllipse (cx - rad, cy - rad, rad * 2, rad * 2, 3.0f);

        const float a0 = (float) std::fmod (now * 0.0055, juce::MathConstants<double>::twoPi);
        juce::Path arc;
        arc.addCentredArc (cx, cy, rad, rad, 0.0f, a0, a0 + 4.2f, true);
        g.setColour (theme::accentHover);
        g.strokePath (arc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else if (showResult)
    {
        g.setColour (resultColour.withAlpha (0.55f + 0.45f * (1.0f - blink * 0.6f)));
        g.fillEllipse (cx - rad, cy - rad, rad * 2, rad * 2);
        g.setColour (juce::Colours::white);

        if (state == State::finished)
        {
            juce::Path tick;
            tick.startNewSubPath (cx - 7.0f, cy + 0.5f);
            tick.lineTo (cx - 2.0f, cy + 5.5f);
            tick.lineTo (cx + 7.5f, cy - 5.5f);
            g.strokePath (tick, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else
        {
            g.fillRoundedRectangle (cx - 6.0f, cy - 6.0f, 12.0f, 12.0f, 2.0f);   // stop square
        }
    }
    else
    {
        g.setColour (theme::outlineStrong);
        g.drawEllipse (cx - rad, cy - rad, rad * 2, rad * 2, 2.0f);
        g.setColour (theme::textDim);
        g.fillEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
    }

    //== right side: big number ==============================================
    auto right = inner.removeFromRight (150);
    auto rightTop = right.removeFromTop (30);

    juce::String big, small;

    if (isBusy())
    {
        if (state == State::scanning)
        {
            big = juce::String (juce::roundToInt (displayedFraction * 100.0f)) + "%";
            small = juce::String (doneFiles) + " / " + juce::String (totalFiles) + " files";
        }
        else
        {
            big = "...";
            small = state == State::saving ? "saving" : "searching";
        }

        const int secs = (int) (elapsedMs / 1000.0);
        small += "  " + juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2);
    }
    else if (showResult)
    {
        big = (newFound > 0 ? "+" : "") + juce::String (newFound);
        small = newFound == 1 ? "new plugin" : "new plugins";
    }
    else
    {
        big = juce::String (totalPlugins);
        small = totalPlugins == 1 ? "plugin" : "plugins";
    }

    g.setColour (showResult ? resultColour : theme::text);
    g.setFont (theme::font (22.0f, true));
    g.drawText (big, rightTop, juce::Justification::centredRight, false);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.5f));
    g.drawText (small, right.removeFromTop (18), juce::Justification::centredRight, false);

    //== title + subtitle ====================================================
    inner.removeFromRight (12);
    auto text = inner.removeFromTop (inner.getHeight() - 20);
    auto titleArea = text.removeFromTop (text.getHeight() / 2 + 2);

    juce::String title, subtitle;

    switch (state)
    {
        case State::idle:
            title = "Ready";
            subtitle = "Rescan looks for new plugins only. Add Folder scans just the folder you choose.";
            break;
        case State::searching:
            title = "Searching plugin folders...";
            subtitle = currentFormat.isNotEmpty() ? "Looking for new " + currentFormat + " plugins" : "Looking for new plugins";
            break;
        case State::scanning:
            title = "Scanning new plugins...";
            subtitle = (currentFormat.isNotEmpty() ? currentFormat + "  -  " : juce::String()) + currentItem
                     + "   |   " + juce::String (newFound) + " new found";
            break;
        case State::saving:
            title = "Saving plugin list...";
            subtitle = juce::String (newFound) + " new plugins found";
            break;
        case State::finished:
            title = newFound > 0 ? "Scan complete" : "Up to date";
            subtitle = newFound > 0
                         ? juce::String (newFound) + (newFound == 1 ? " new plugin" : " new plugins")
                             + " added  -  " + juce::String (totalPlugins) + " total"
                         : "No new plugins found  -  " + juce::String (totalPlugins) + " total";
            if (failedCount > 0)
                subtitle += "  -  " + juce::String (failedCount) + " could not be loaded";
            break;
        case State::cancelled:
            title = "Scan stopped";
            subtitle = juce::String (newFound) + " new plugins kept  -  " + juce::String (totalPlugins) + " total";
            break;
    }

    g.setColour (showResult ? resultColour.brighter (0.25f) : theme::text);
    g.setFont (theme::font (15.5f, true));
    g.drawText (title, titleArea, juce::Justification::centredLeft, true);

    g.setColour (theme::textDim);
    g.setFont (theme::font (13.0f));
    g.drawText (subtitle, text, juce::Justification::centredLeft, true);

    //== progress bar ========================================================
    auto bar = getLocalBounds().reduced (16, 0).removeFromBottom (18).removeFromTop (8).toFloat();
    bar.setY ((float) getHeight() - 20.0f);

    g.setColour (theme::stage);
    g.fillRoundedRectangle (bar, 4.0f);

    juce::Rectangle<float> fill;

    if (isBusy())
    {
        if (state == State::scanning)
            fill = bar.withWidth (juce::jmax (8.0f, bar.getWidth() * displayedFraction));
        else
        {
            const float segW = bar.getWidth() * 0.28f;
            const float pos  = 0.5f - 0.5f * (float) std::cos (now * 0.0040);
            fill = juce::Rectangle<float> (bar.getX() + (bar.getWidth() - segW) * pos, bar.getY(), segW, bar.getHeight());
        }
    }
    else if (showResult)
        fill = bar;

    if (! fill.isEmpty())
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip;
        clip.addRoundedRectangle (fill, 4.0f);
        g.reduceClipRegion (clip);

        const auto c1 = showResult ? resultColour.darker (0.2f) : theme::accentDeep;
        const auto c2 = showResult ? resultColour.brighter (0.15f) : theme::accentHover;
        g.setGradientFill (juce::ColourGradient (c1, fill.getX(), 0.0f, c2, fill.getRight(), 0.0f, false));
        g.fillRect (fill);

        if (isBusy())       // moving diagonal stripes + glowing tip
        {
            const float off = (float) std::fmod (now * 0.045, 22.0);
            g.setColour (juce::Colours::white.withAlpha (0.16f));
            for (float x = fill.getX() - 12.0f; x < fill.getRight() + 12.0f; x += 22.0f)
                g.drawLine (x + off, fill.getBottom() + 1.0f, x + off + 8.0f, fill.getY() - 1.0f, 6.0f);

            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.55f), fill.getRight(), fill.getCentreY(),
                                                     juce::Colours::transparentWhite, fill.getRight() - 26.0f, fill.getCentreY(), false));
            g.fillRect (fill);
        }
    }
}

//==============================================================================
// Dialog
//==============================================================================
PluginBrowserDialog::PluginBrowserDialog (PluginScanner& scanner)
    : pluginScanner (scanner)
{
    setSize (1000, 640);

    //== toolbar =============================================================
    searchBox.setFont (theme::font (szRow));
    searchBox.setTextToShowWhenEmpty ("Search name, vendor, category...", theme::textFaint);
    searchBox.setJustification (juce::Justification::centredLeft);   // vertically centred in the box
    searchBox.setIndents (12, 0);
    searchBox.setEscapeAndReturnKeysConsumed (false);
    searchBox.onTextChange = [this] { rebuildFilteredList(); };

    categoryFilter.addItem ("All Categories", 1);
    categoryFilter.addItem ("Instruments", 2);
    categoryFilter.addItem ("Effects", 3);
    {
        int id = 4;
        for (auto& def : categoryDefs())
            categoryFilter.addItem (def.label, id++);
        categoryFilter.addItem ("Other", id);
    }
    categoryFilter.setSelectedId (1, juce::dontSendNotification);
    categoryFilter.onChange = [this] { rebuildFilteredList(); };

    rescanBtn.setTooltip ("Look for new or updated plugins (existing plugins are kept)");
    rescanBtn.onClick = [this]
    {
        if (pluginScanner.isScanning())
            pluginScanner.cancelScan();
        else
            confirmThenScan ([this] { startScanUI(); });
    };

    retryFailedBtn.setButtonText (juce::String::fromUTF8 ("Thử lại plugin lỗi"));
    retryFailedBtn.setTooltip (juce::String::fromUTF8 ("Quét lại cả những plugin trước đó bị bỏ qua vì lỗi hoặc làm app đóng"));
    retryFailedBtn.onClick = [this]
    {
        if (pluginScanner.isScanning())
            return;

        confirmThenScan ([this]
        {
            pluginScanner.clearFailedPlugins();
            startScanUI();
        });
    };

    addFolderBtn.setTooltip ("Scan only the folder you choose");
    addFolderBtn.onClick = [this]
    {
        folderChooser = std::make_unique<juce::FileChooser> ("Select Plugin Folder", juce::File(), "*.*", true);
        folderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto folder = fc.getResult();
                if (folder != juce::File() && folder.isDirectory())
                    confirmThenScan ([this, folder] { startScanUI ({ folder.getFullPathName() }, true); });   // ONLY this folder
            });
    };

    //== table ===============================================================
    auto& header = table.getHeader();
    header.setLookAndFeel (&headerLnF);
    header.addColumn ("Name",         colName,     280, 140, 600, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("Vendor",       colVendor,   170,  90, 360, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("Category",     colCategory, 200,  90, 400, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("VST",          colVst,       76,  60, 120, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("Version",      colVersion,  100,  70, 200, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("Architecture", colArch,     130, 110, 200, juce::TableHeaderComponent::defaultFlags);
    header.setStretchToFitActive (true);
    header.setSortColumnId (colName, true);

    table.setHeaderHeight (headerH);
    table.setRowHeight (rowH);
    table.setColour (juce::ListBox::backgroundColourId, theme::stage);
    table.setColour (juce::ListBox::outlineColourId, theme::outline);
    table.setOutlineThickness (1);
    table.getViewport()->setScrollBarsShown (true, false);

    //== bottom ==============================================================
    countLabel.setFont (theme::font (13.0f));
    countLabel.setColour (juce::Label::textColourId, theme::textDim);
    countLabel.setJustificationType (juce::Justification::centredLeft);

    closeBtn.onClick = [this] { closeButtonPressed(); };

    addAndMakeVisible (searchBox);
    addAndMakeVisible (categoryFilter);
    addAndMakeVisible (rescanBtn);
    addAndMakeVisible (addFolderBtn);
    addAndMakeVisible (retryFailedBtn);
    addAndMakeVisible (table);
    addAndMakeVisible (statusPanel);
    addAndMakeVisible (countLabel);
    addAndMakeVisible (closeBtn);

    // Wire scanner completion
    pluginScanner.onScanComplete = [this] { onScanFinished(); };

    wasScanning = pluginScanner.isScanning();
    finishedHandled = ! wasScanning;
    rebuildFilteredList();
    statusPanel.setIdle (totalPluginCount);

    // Realtime progress: poll the scanner (also covers a scan that was already started
    // before this dialog opened, e.g. the very first automatic scan).
    startTimerHz (15);
    updateProgressUI();
}

PluginBrowserDialog::~PluginBrowserDialog()
{
    stopTimer();
    table.getHeader().setLookAndFeel (nullptr);
    // Detach callback so a still-running scan doesn't call into a dead object.
    pluginScanner.onScanComplete = nullptr;
}

void PluginBrowserDialog::closeButtonPressed()
{
    if (auto* pw = findParentComponentOfClass<juce::DialogWindow>())
        pw->exitModalState (0);
}

//==============================================================================
// Scan control / progress
//==============================================================================
void PluginBrowserDialog::confirmThenScan (std::function<void()> go)
{
    if (! (isAudioRunning && isAudioRunning()))
    {
        go();
        return;
    }

    juce::Component::SafePointer<PluginBrowserDialog> safe (this);

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("pt.Live")
                                      .withMessage (juce::String::fromUTF8 ("Quét plugin có thể gây giật tiếng và đôi khi làm app đóng. Tiếp tục?"))
                                      .withButton (juce::String::fromUTF8 ("Quét"))
                                      .withButton (juce::String::fromUTF8 ("Hủy")),
                                  [safe, go] (int result)
                                  {
                                      if (safe != nullptr && result == 1)
                                          go();
                                  });
}

void PluginBrowserDialog::startScanUI (const juce::StringArray& paths, bool onlyGivenPaths)
{
    if (pluginScanner.isScanning())
        return;

    // Snapshot of what we already have, so plugins found by THIS scan get a "NEW" tag.
    knownBeforeScan.clear();
    for (const auto& d : pluginScanner.getScannedPlugins())
        knownBeforeScan.insert (rowKey (d));
    highlightNew = ! knownBeforeScan.empty();

    finishedHandled = false;
    lastFoundCount = -1;
    wasScanning = true;

    pluginScanner.startScan (paths, onlyGivenPaths);
    statusPanel.setProgress (PluginScanner::Phase::searching, 0, 0, 0, {}, {});
    updateButtons();
    updateProgressUI();      // show feedback immediately, don't wait for the first tick
}

void PluginBrowserDialog::updateButtons()
{
    const bool scanning = pluginScanner.isScanning();

    addFolderBtn.setEnabled (! scanning);
    retryFailedBtn.setEnabled (! scanning);

    if (scanning)
    {
        rescanBtn.setButtonText ("Stop");
        rescanBtn.setColour (juce::TextButton::buttonColourId, theme::danger);
    }
    else
    {
        rescanBtn.setButtonText ("Rescan");
        rescanBtn.removeColour (juce::TextButton::buttonColourId);
    }
}

void PluginBrowserDialog::timerCallback()
{
    updateProgressUI();
}

void PluginBrowserDialog::updateProgressUI()
{
    const bool scanning = pluginScanner.isScanning();

    if (scanning && ! wasScanning)          // scan started elsewhere (first automatic scan)
    {
        finishedHandled = false;
        updateButtons();
    }

    if (scanning)
    {
        statusPanel.setProgress (pluginScanner.getPhase(),
                                 pluginScanner.getDoneFiles(), pluginScanner.getTotalFiles(),
                                 pluginScanner.getNewCount(),
                                 pluginScanner.getCurrentItem(), pluginScanner.getCurrentFormat());

        // Fill the list live as plugins are discovered.
        const int found = pluginScanner.getFoundCount();
        if (found != lastFoundCount)
        {
            lastFoundCount = found;
            rebuildFilteredList();
        }
    }
    else if (wasScanning)
    {
        onScanFinished();
    }

    wasScanning = scanning;
}

void PluginBrowserDialog::onScanFinished()
{
    if (finishedHandled)
        return;

    finishedHandled = true;
    lastFoundCount = -1;

    rebuildFilteredList();
    statusPanel.showFinished (pluginScanner.getNewCount(), pluginScanner.getFailedCount(),
                              totalPluginCount, pluginScanner.wasCancelled());
    updateButtons();
}

//==============================================================================
// Layout / paint
//==============================================================================
void PluginBrowserDialog::resized()
{
    auto r = getLocalBounds().reduced (16);

    auto toolbar = r.removeFromTop (toolbarH);
    addFolderBtn.setBounds (toolbar.removeFromRight (140));
    toolbar.removeFromRight (8);
    retryFailedBtn.setBounds (toolbar.removeFromRight (150));
    toolbar.removeFromRight (8);
    rescanBtn.setBounds (toolbar.removeFromRight (110));
    toolbar.removeFromRight (8);
    categoryFilter.setBounds (toolbar.removeFromRight (200));
    toolbar.removeFromRight (8);
    searchBox.setBounds (toolbar);

    r.removeFromTop (12);

    auto bottom = r.removeFromBottom (bottomH);
    closeBtn.setBounds (bottom.removeFromRight (110));
    countLabel.setBounds (bottom);

    r.removeFromBottom (8);
    statusPanel.setBounds (r.removeFromBottom (statusH));
    r.removeFromBottom (12);

    table.setBounds (r);
    table.getHeader().resizeAllColumnsToFit (juce::jmax (100, table.getViewport()->getMaximumVisibleWidth()));
}

void PluginBrowserDialog::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
}

void PluginBrowserDialog::paintOverChildren (juce::Graphics& g)
{
    if (! rows.empty())
        return;

    auto area = table.getBounds().withTrimmedTop (headerH);

    g.setColour (theme::textDim);
    g.setFont (theme::font (15.0f));

    juce::String msg;

    if (pluginScanner.isScanning())
        msg = "Looking for plugins...";
    else if (totalPluginCount == 0)
        msg = "No plugins yet.\nPress Rescan, or Add Folder to scan a specific folder.";
    else
        msg = "No plugins match your search.";

    g.drawFittedText (msg, area.reduced (24), juce::Justification::centred, 3);
}

//==============================================================================
// Table model
//==============================================================================
int PluginBrowserDialog::getNumRows()
{
    return (int) rows.size();
}

void PluginBrowserDialog::paintRowBackground (juce::Graphics& g, int row, int width, int height, bool selected)
{
    if (selected)
        g.fillAll (theme::selection);
    else if (row % 2 == 1)
        g.fillAll (theme::laneFill);

    g.setColour (theme::hairline);
    g.fillRect (0, height - 1, width, 1);
}

void PluginBrowserDialog::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                     int width, int height, bool /*selected*/)
{
    if (rowNumber < 0 || rowNumber >= (int) rows.size())
        return;

    const auto& row  = rows[(size_t) rowNumber];
    const auto& desc = row.desc;
    auto area = juce::Rectangle<int> (0, 0, width, height).reduced (12, 0);

    switch (columnId)
    {
        case colName:
        {
            if (row.isNew)
            {
                auto pill = area.removeFromRight (40).withSizeKeepingCentre (36, 18).toFloat();
                drawPill (g, pill, theme::success.withAlpha (0.25f), theme::success.brighter (0.3f), "NEW", 11.0f);
                area.removeFromRight (6);
            }

            g.setColour (theme::text);
            g.setFont (theme::font (szRow));
            g.drawText (desc.name, area, juce::Justification::centredLeft, true);
            break;
        }

        case colVendor:
            g.setColour (theme::textDim);
            g.setFont (theme::font (szCell2));
            g.drawText (desc.manufacturerName.isNotEmpty() ? desc.manufacturerName : "-",
                        area, juce::Justification::centredLeft, true);
            break;

        case colCategory:
            g.setColour (theme::textDim);
            g.setFont (theme::font (szCell2));
            g.drawText (row.category, area, juce::Justification::centredLeft, true);
            break;

        case colVst:
        {
            const bool v3 = vstLabel (desc) == "VST3";
            auto pill = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 48), 20).withX (area.getX()).toFloat();
            drawPill (g, pill,
                      v3 ? theme::accent.withAlpha (0.22f) : theme::warning.withAlpha (0.20f),
                      v3 ? theme::accentHover : theme::warningHover,
                      vstLabel (desc), 12.0f);
            break;
        }

        case colVersion:
            g.setColour (theme::textDim);
            g.setFont (theme::font (szCell2));
            g.drawText (desc.version.isNotEmpty() ? desc.version : "-", area, juce::Justification::centredLeft, true);
            break;

        case colArch:
        {
            const auto& a = row.architecture;
            // 32-bit plugins need a bridge (e.g. jBridge): highlight them.
            auto colour = a.startsWith ("32") ? theme::warning
                        : a == "ARM64"        ? theme::accent
                        : theme::textDim;

            g.setColour (colour);
            g.setFont (theme::font (szCell2));
            g.drawText (a, area, juce::Justification::centredLeft, true);
            break;
        }

        default: break;
    }
}

void PluginBrowserDialog::sortOrderChanged (int newSortColumnId, bool isForwards)
{
    sortColumn = newSortColumnId;
    sortForwards = isForwards;
    applySort();
    table.updateContent();
    table.repaint();
}

//==============================================================================
// Data
//==============================================================================
juce::String PluginBrowserDialog::architectureFor (const juce::PluginDescription& d)
{
    const auto key = d.fileOrIdentifier;

    if (auto it = archCache.find (key); it != archCache.end())
        return it->second;

    auto a = detectArchitecture (d);
    archCache[key] = a;
    return a;
}

void PluginBrowserDialog::refreshList()
{
    rebuildFilteredList();
}

void PluginBrowserDialog::applySort()
{
    auto field = [this] (const Row& r) -> juce::String
    {
        switch (sortColumn)
        {
            case colVendor:   return r.desc.manufacturerName;
            case colCategory: return r.category;
            case colVst:      return vstLabel (r.desc);
            case colVersion:  return r.desc.version;
            case colArch:     return r.architecture;
            default:          return r.desc.name;
        }
    };

    std::stable_sort (rows.begin(), rows.end(), [&] (const Row& a, const Row& b)
    {
        int c = field (a).compareNatural (field (b));

        if (c == 0)
            c = a.desc.name.compareNatural (b.desc.name);

        return sortForwards ? c < 0 : c > 0;
    });
}

void PluginBrowserDialog::rebuildFilteredList()
{
    const auto all = pluginScanner.getScannedPlugins();
    const int cat = categoryFilter.getSelectedId();
    const auto tokens = juce::StringArray::fromTokens (searchBox.getText().trim(), " ", "\"");

    rows.clear();

    for (const auto& d : all)
    {
        if (! matchesCategory (d, cat))
            continue;

        Row r;
        r.desc = d;
        r.category = prettyCategory (d);

        bool ok = true;
        for (const auto& t : tokens)
        {
            const auto hay = d.name + " " + d.manufacturerName + " " + r.category + " " + d.version
                           + " " + vstLabel (d);
            if (! hay.containsIgnoreCase (t)) { ok = false; break; }
        }
        if (! ok)
            continue;

        r.architecture = architectureFor (d);
        r.isNew = highlightNew && knownBeforeScan.find (rowKey (d)) == knownBeforeScan.end();
        rows.push_back (std::move (r));
    }

    totalPluginCount = all.size();

    applySort();
    table.updateContent();
    table.repaint();
    updateCountLabel();
    repaint();
}

void PluginBrowserDialog::updateCountLabel()
{
    const int shown = (int) rows.size();

    countLabel.setText (shown == totalPluginCount
                            ? juce::String (totalPluginCount) + (totalPluginCount == 1 ? " plugin" : " plugins")
                            : "Showing " + juce::String (shown) + " of " + juce::String (totalPluginCount) + " plugins",
                        juce::dontSendNotification);
}
