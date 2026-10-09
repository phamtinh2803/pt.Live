/*
    pt.Live - Live Singing DAW
    Central Tracktion Engine include + namespace alias.

    The Tracktion Engine headers wrap everything in `namespace tracktion::inline engine`,
    so `te::X` resolves via this alias.
*/

#pragma once

#include <JuceHeader.h>
#include "tracktion_engine/tracktion_engine.h"

namespace te = tracktion::engine;
