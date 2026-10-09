/*
    pt.Live - Live Singing DAW
    Platform glue for plugin editor windows (see include/Audio/PluginWindow.h).
*/

#include <JuceHeader.h>
#include "include/Audio/PluginWindow.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace ptl::pluginhost
{
    void setOwnerWindow (juce::Component& child, juce::Component* owner)
    {
       #if JUCE_WINDOWS
        auto childHwnd = static_cast<HWND> (child.getWindowHandle());
        auto ownerHwnd = owner != nullptr ? static_cast<HWND> (owner->getWindowHandle()) : nullptr;

        if (childHwnd != nullptr && ownerHwnd != nullptr)
        {
            // GWLP_HWNDPARENT on a top-level window sets its OWNER (not a child parent):
            // an owned window is always above its owner and follows its minimise / restore.
            SetWindowLongPtr (childHwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR> (ownerHwnd));
            return;
        }
       #endif

        juce::ignoreUnused (owner);
        child.setAlwaysOnTop (true);
    }
}
