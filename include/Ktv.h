/*
    pt.Live - Live Singing DAW
    KTV (technician) mode: a "factory" copy of the session, protected by the technician's password.

    Everything lives INSIDE the .pt itself (as properties on the Edit's root state), so it travels
    with the file and is specific to that file:

        ptlKtvSalt / ptlKtvHash   password of the technician (salted, iterated SHA-256 - never the text)
        ptlKtvBase                the KTV copy of the whole session (gzip + base64)

    The password only guards CHANGING the KTV data (saving a new KTV copy, changing / removing the
    password). The user can do anything else, and the title bar RESET brings the file back to the KTV
    copy without a password.

    NOTE: this protects against a user who clicks around, not against someone who edits the XML by
    hand or reinstalls the app. That is the intended level.
*/

#pragma once

#include <JuceHeader.h>
#include <functional>

namespace ptl::ktv
{
    // ---- state helpers (operate on the Edit's root ValueTree) ---------------
    bool hasKtv        (const juce::ValueTree& editState);          // a KTV password exists in this file
    bool hasBase       (const juce::ValueTree& editState);          // a KTV copy exists in this file

    bool checkPassword (const juce::ValueTree& editState, const juce::String& password);
    void setPassword   (juce::ValueTree& editState, const juce::String& password);
    void clearAll      (juce::ValueTree& editState);                // removes password and KTV copy

    // Stores the KTV copy: `savedFile` must be the .pt that was JUST written from `editState`
    // (it holds the complete plugin state). Returns false when the file cannot be read.
    bool storeBase (juce::ValueTree& editState, const juce::File& savedFile);

    // Writes the KTV copy over `target` (the KTV password and copy of `liveState` are kept).
    // The old file is only replaced when the new one was written completely.
    bool writeRestoredFile (const juce::ValueTree& liveState, const juce::File& target);

    // ---- small dialogs ----------------------------------------------------------
    // Password box. confirmNew = ask twice (creating / changing a password).
    // onOk gets a non-empty password; nothing is called on Cancel / mismatch.
    void askPassword (juce::Component* parent, const juce::String& title, const juce::String& message,
                      bool confirmNew, std::function<void (const juce::String&)> onOk);

    // Yes / No question. onYes is called only on the OK button.
    void confirm (juce::Component* parent, const juce::String& title, const juce::String& message,
                  const juce::String& okText, std::function<void()> onYes);

    void info (juce::Component* parent, const juce::String& message, bool warning = false);
}
