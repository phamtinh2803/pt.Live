/*
    pt.Live - Live Singing DAW
    HistoryPanel - the "DANH SÁCH" popup: the songs opened so far, newest first.

    - Shows 10 rows at a time with a big readable font; a scroll bar reaches the older songs.
    - Search box on top: type with OR without Vietnamese accents ("ngay" finds "NGÀY MAI ..."), several words
      narrow the result (all words must match, in any order). The key (e.g. "Bb minor") is searched too.
      Enter opens the first result.
    - Click a row  -> onPick (url) and the popup closes.
    - Footer button clears the list (needs a second click to confirm).
    Shown inside a juce::CallOutBox (see MainWindow::showHistoryPopup).
*/

#pragma once

#include <JuceHeader.h>
#include <map>
#include "include/Theme.h"
#include "include/Audio/SongHistory.h"
#include "include/Audio/LiveController.h"

class HistoryPanel : public juce::Component,
                     private juce::ListBoxModel
{
public:
    static constexpr int visibleRows = 10;
    static constexpr int rowH        = 40;
    static constexpr int headerH     = 36;
    static constexpr int searchH     = 46;
    static constexpr int footerH     = 46;
    static constexpr int panelW      = 560;

    HistoryPanel (juce::Array<ptl::SongEntry> songList,
                  std::function<void (const juce::String& url)> onPickSong,
                  std::function<void()> onClearAll)
        : songs (std::move (songList)), onPick (std::move (onPickSong)), onClear (std::move (onClearAll))
    {
        for (int i = 0; i < songs.size(); ++i)
            shown.add (i);

        search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Tìm bài hát... (có dấu hoặc không dấu đều được)"),
                                       ptl::theme::textFaint);
        search.setFont (ptl::theme::font (16.0f));
        search.setColour (juce::TextEditor::backgroundColourId, ptl::theme::stage);
        search.setColour (juce::TextEditor::textColourId, ptl::theme::text);
        search.setColour (juce::TextEditor::outlineColourId, ptl::theme::outline);
        search.setColour (juce::TextEditor::focusedOutlineColourId, ptl::theme::accent);
        search.setColour (juce::CaretComponent::caretColourId, ptl::theme::accent);
        search.setMultiLine (false);
        search.setBorder (juce::BorderSize<int> (0));                       // default border is lopsided (3 px at the bottom)
        search.setJustification (juce::Justification::centredLeft);         // text AND hint centred vertically in the box
        search.setIndents (10, 0);
        search.setScrollbarsShown (false);
        search.onTextChange = [this] { applyFilter(); };
        search.onReturnKey  = [this] { if (shown.size() > 0) pickRow (0); };
        addAndMakeVisible (search);

        list.setModel (this);
        list.setRowHeight (rowH);
        list.setOutlineThickness (0);
        list.setMouseMoveSelectsRows (true);
        list.setColour (juce::ListBox::backgroundColourId, ptl::theme::stage);
        list.getVerticalScrollBar().setAutoHide (false);      // the scroll bar is always visible
        addAndMakeVisible (list);

        clearBtn.setButtonText (juce::String::fromUTF8 ("Xóa toàn bộ danh sách"));
        clearBtn.setColour (juce::TextButton::buttonColourId, ptl::theme::raised);
        clearBtn.setColour (juce::TextButton::textColourOffId, ptl::theme::textDim);
        clearBtn.onClick = [this] { onClearClicked(); };
        addAndMakeVisible (clearBtn);

        const int rows = juce::jlimit (1, visibleRows, songs.size());
        setSize (panelW, headerH + searchH + rows * rowH + footerH);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace ptl::theme;
        g.fillAll (panel);

        g.setColour (text);
        g.setFont (font (17.0f, true));
        g.drawText (juce::String::fromUTF8 ("Danh sách bài hát (mới nhất lên đầu)"),
                    juce::Rectangle<int> (14, 0, getWidth() - 28, headerH), juce::Justification::centredLeft, true);
        g.setColour (hairline);
        g.fillRect (0, headerH - 1, getWidth(), 1);

        if (shown.isEmpty())
        {
            g.setColour (textFaint);
            g.setFont (font (16.0f));
            g.drawText (songs.isEmpty() ? juce::String::fromUTF8 ("(chưa có bài hát nào được mở)")
                                        : juce::String::fromUTF8 ("Không tìm thấy bài nào"),
                        juce::Rectangle<int> (0, headerH + searchH, getWidth(), rowH), juce::Justification::centred, true);
        }
    }

    void parentHierarchyChanged() override
    {
        if (getParentComponent() != nullptr)
        {
            juce::Component::SafePointer<juce::TextEditor> ed (&search);
            juce::MessageManager::callAsync ([ed] { if (ed != nullptr) ed->grabKeyboardFocus(); });
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (headerH);
        search.setBounds (r.removeFromTop (searchH).reduced (12, 6));
        auto foot = r.removeFromBottom (footerH).reduced (12, 7);
        clearBtn.setBounds (foot);
        list.setBounds (r);
    }

private:
    //== ListBoxModel ===========================================================================
    int getNumRows() override { return shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        using namespace ptl::theme;
        if (! juce::isPositiveAndBelow (row, shown.size())) return;
        const int orig = shown.getUnchecked (row);
        const auto& s = songs.getReference (orig);

        if (selected)
        {
            g.setColour (accentDeep.withAlpha (0.45f));
            g.fillRect (0, 0, w, h);
        }
        g.setColour (hairline);
        g.fillRect (0, h - 1, w, 1);

        // key tag on the right: "Bb MINOR"
        int tagW = 0;
        if (s.detectedRoot.isNotEmpty() && s.detectedScale.isNotEmpty())
        {
            const auto tag = LiveController::displayRootName (s.detectedRoot, s.detectedScale) + " " + s.detectedScale.toUpperCase();
            const auto tf  = font (15.0f, true);
            tagW = juce::GlyphArrangement::getStringWidthInt (tf, tag) + 8;
            g.setColour (selected ? accentHover : accent);
            g.setFont (tf);
            g.drawText (tag, w - 18 - tagW - 8, 0, tagW, h, juce::Justification::centredRight, false);   // 18 = scroll bar
            tagW += 8;
        }

        // number
        g.setColour (textFaint);
        g.setFont (font (14.0f));
        g.drawText ("#" + juce::String (orig + 1), 10, 0, 44, h, juce::Justification::centredLeft, false);

        // title (elided)
        g.setColour (selected ? text : text.withAlpha (0.9f));
        g.setFont (font (16.0f));
        g.drawText (s.title.isNotEmpty() ? s.title : s.url,
                    juce::Rectangle<int> (58, 0, w - 58 - 18 - tagW - 12, h), juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        pickRow (row);
    }

    //== search =================================================================================
    // lower case, no Vietnamese accents ("Ngày" / "ngay" / "NGÀY" all become "ngay"; đ -> d)
    static juce::String normalise (const juce::String& s)
    {
        // accent -> base letter, built once
        static const std::map<juce::juce_wchar, juce::juce_wchar> accentMap = []
        {
            struct Row { const char* accented; char base; };
            static const Row rows[] =
            {
                { "àáạảãâầấậẩẫăằắặẳẵÀÁẠẢÃÂẦẤẬẨẪĂẰẮẶẲẴ", 'a' },
                { "èéẹẻẽêềếệểễÈÉẸẺẼÊỀẾỆỂỄ",               'e' },
                { "ìíịỉĩÌÍỊỈĨ",                           'i' },
                { "òóọỏõôồốộổỗơờớợởỡÒÓỌỎÕÔỒỐỘỔỖƠỜỚỢỞỠ", 'o' },
                { "ùúụủũưừứựửữÙÚỤỦŨƯỪỨỰỬỮ",               'u' },
                { "ỳýỵỷỹỲÝỴỶỸ",                           'y' },
                { "đĐ",                                    'd' }
            };

            std::map<juce::juce_wchar, juce::juce_wchar> m;
            for (auto& r : rows)
                for (auto p = juce::String::fromUTF8 (r.accented).getCharPointer(); ! p.isEmpty(); ++p)
                    m[*p] = (juce::juce_wchar) r.base;
            return m;
        }();

        juce::String out;
        for (auto p = s.getCharPointer(); ! p.isEmpty(); ++p)
        {
            const juce::juce_wchar c = *p;
            if (c >= 0x0300 && c <= 0x036F) continue;            // combining accent marks (decomposed text)

            const auto it = accentMap.find (c);
            out += (it != accentMap.end()) ? it->second : juce::CharacterFunctions::toLowerCase (c);
        }
        return out;
    }

    void applyFilter()
    {
        const auto words = juce::StringArray::fromTokens (normalise (search.getText()).trim(), " ", "");

        shown.clear();
        for (int i = 0; i < songs.size(); ++i)
        {
            const auto& s = songs.getReference (i);
            const auto hay = normalise (s.title + " " + s.url + " "
                                        + LiveController::displayRootName (s.detectedRoot, s.detectedScale) + " " + s.detectedScale);
            bool all = true;
            for (auto& w : words)
                if (w.isNotEmpty() && ! hay.contains (w)) { all = false; break; }
            if (all) shown.add (i);
        }

        list.updateContent();
        list.deselectAllRows();
        list.scrollToEnsureRowIsOnscreen (0);
        repaint();
    }

    void pickRow (int row)
    {
        if (! juce::isPositiveAndBelow (row, shown.size())) return;
        const auto url = songs.getReference (shown.getUnchecked (row)).url;
        auto cb = onPick;
        finish ([cb, url] { if (cb) cb (url); });
    }

    //== helpers ================================================================================
    void onClearClicked()
    {
        if (! confirmClear)
        {
            confirmClear = true;
            clearBtn.setButtonText (juce::String::fromUTF8 ("Bấm lần nữa để xóa hết"));
            clearBtn.setColour (juce::TextButton::buttonColourId, ptl::theme::danger);
            clearBtn.setColour (juce::TextButton::textColourOffId, ptl::theme::textOnAccent);
            return;
        }
        auto cb = onClear;
        finish ([cb] { if (cb) cb(); });
    }

    // Closing the call-out deletes this panel, so the action only uses copies and runs afterwards.
    void finish (std::function<void()> action)
    {
        juce::Component::SafePointer<juce::CallOutBox> box (findParentComponentOfClass<juce::CallOutBox>());
        juce::MessageManager::callAsync ([box, action]
        {
            if (box != nullptr) box->dismiss();
            if (action) action();
        });
    }

    juce::Array<ptl::SongEntry> songs;
    std::function<void (const juce::String&)> onPick;
    std::function<void()> onClear;
    juce::Array<int> shown;                 // indices into `songs` that match the search
    juce::TextEditor search;
    juce::ListBox list;
    juce::TextButton clearBtn;
    bool confirmClear = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HistoryPanel)
};
