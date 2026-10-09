/*
    pt.Live - Live Singing DAW
    Plugin Browser Dialog

    Professional plugin table: Name | Vendor | Category | VST | Version | Architecture
      - search box + category filter, click a header to sort, drag header edges to resize
      - "Rescan" only looks for NEW / updated plugins (the existing list is kept)
      - "Add Folder..." scans ONLY the folder the user picked
      - animated scan status panel, flashes for 2 s when a scan completes
*/

#pragma once

#include <JuceHeader.h>
#include <map>
#include <set>
#include <vector>
#include "include/UI/PluginScanner.h"

//==============================================================================
// Animated scan status card (spinner / progress bar / result flash).
class PluginScanStatusPanel : public juce::Component,
                              private juce::Timer
{
public:
    PluginScanStatusPanel();
    ~PluginScanStatusPanel() override { stopTimer(); }

    void paint (juce::Graphics&) override;

    // Nothing running: shows the total number of plugins.
    void setIdle (int totalPlugins);

    // Scan running. total <= 0 (or searching phase) = indeterminate bar.
    void setProgress (PluginScanner::Phase phase, int done, int total, int newFound,
                      const juce::String& currentItem, const juce::String& format);

    // Scan finished: flashes for 2 seconds, then stays as a steady result card.
    void showFinished (int newCount, int failedCount, int totalPlugins, bool cancelled);

private:
    enum class State { idle, searching, scanning, saving, finished, cancelled };

    void timerCallback() override;
    bool isBusy() const noexcept   { return state == State::searching || state == State::scanning || state == State::saving; }
    bool isBlinking() const noexcept;
    void ensureTimer();

    State state = State::idle;

    int doneFiles = 0, totalFiles = 0, newFound = 0, totalPlugins = 0, failedCount = 0;
    juce::String currentItem, currentFormat;

    float displayedFraction = 0.0f;
    double busyStartMs = 0.0, elapsedMs = 0.0;
    double blinkStartMs = 0.0;
    juce::Colour resultColour;

    static constexpr double blinkDurationMs = 2000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScanStatusPanel)
};

//==============================================================================
class PluginBrowserDialog : public juce::Component,
                            public juce::TableListBoxModel,
                            private juce::Timer
{
public:
    // Set by the owner: true while an audio device is running (a scan can then make the sound stutter).
    std::function<bool()> isAudioRunning;

    explicit PluginBrowserDialog (PluginScanner& scanner);
    ~PluginBrowserDialog() override;

    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;

    // TableListBoxModel
    int  getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int columnId, int width, int height, bool selected) override;
    void sortOrderChanged (int newSortColumnId, bool isForwards) override;

    // Refresh list from scanner results
    void refreshList();

private:
    struct Row
    {
        juce::PluginDescription desc;
        juce::String category;      // prettified
        juce::String architecture;  // 32-bit / 64-bit / ARM64 / ...
        bool isNew = false;         // found by the last scan started from this dialog
    };

    enum ColumnId { colName = 1, colVendor, colCategory, colVst, colVersion, colArch };

    void timerCallback() override;          // polls scanner progress while scanning
    void updateProgressUI();
    void onScanFinished();
    void startScanUI (const juce::StringArray& paths = {}, bool onlyGivenPaths = false);
    void confirmThenScan (std::function<void()> go);   // asks first when the audio device is running
    void updateButtons();
    void closeButtonPressed();
    void rebuildFilteredList();
    void applySort();
    void updateCountLabel();
    juce::String architectureFor (const juce::PluginDescription&);

    // Header look (own look-and-feel so the table header matches the pt.Live theme).
    struct HeaderLookAndFeel : public juce::LookAndFeel_V4
    {
        void drawTableHeaderBackground (juce::Graphics&, juce::TableHeaderComponent&) override;
        void drawTableHeaderColumn (juce::Graphics&, juce::TableHeaderComponent&, const juce::String& columnName,
                                    int columnId, int width, int height, bool isMouseOver, bool isMouseDown,
                                    int columnFlags) override;
    };

    PluginScanner& pluginScanner;

    HeaderLookAndFeel headerLnF;            // declared before 'table' so it outlives it

    juce::TextEditor searchBox;
    juce::ComboBox categoryFilter;
    juce::TextButton rescanBtn { "Rescan" };
    juce::TextButton addFolderBtn { "Add Folder..." };
    juce::TextButton retryFailedBtn;                    // "Thử lại plugin lỗi": forget skipped plugins and scan again
    juce::TableListBox table { "plugins", this };
    PluginScanStatusPanel statusPanel;
    juce::Label countLabel;
    juce::TextButton closeBtn { "Close" };

    std::unique_ptr<juce::FileChooser> folderChooser;

    std::vector<Row> rows;
    std::map<juce::String, juce::String> archCache;
    std::set<juce::String> knownBeforeScan;     // snapshot used to tag NEW plugins
    bool highlightNew = false;

    int  sortColumn = colName;
    bool sortForwards = true;
    int  totalPluginCount = 0;

    bool wasScanning = false;
    bool finishedHandled = true;
    int  lastFoundCount = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginBrowserDialog)
};
