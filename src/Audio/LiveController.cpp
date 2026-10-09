/*
    pt.Live - Live Singing DAW
    LiveController implementation.  The plugin/parameter helpers were moved here unchanged in
    behaviour from MainWindow.cpp (they used to serve the ptStudio remote commands).
*/

#include "include/Audio/LiveController.h"
#include "include/Audio/PtLog.h"
#include "include/Audio/PluginWindow.h"

#include <map>

namespace
{
    // "Auto-Tune Pro" / "Auto Tune Pro" / "AutoTune Pro" -> "autotunepro".
    juce::String normaliseName (const juce::String& name)
    {
        return name.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789").toLowerCase();
    }

    // Keep '#', turn the Unicode music signs into b / #.
    juce::String plainText (const juce::String& s)
    {
        return s.replace (juce::String::fromUTF8 ("\xe2\x99\xad"), "b")
                .replace (juce::String::fromUTF8 ("\xe2\x99\xaf"), "#")
                .trim().toLowerCase();
    }

    bool pluginNameContains (te::Plugin& p, const juce::String& needle)
    {
        return normaliseName (p.getName()).contains (normaliseName (needle));
    }

    void logTrackPlugins (te::AudioTrack& track, const juce::String& why, bool withParams)
    {
        auto plugins = track.pluginList.getPlugins();
        ptl::diag::write ("[live] " + why + " | track '" + track.getName() + "' has "
                          + juce::String (plugins.size()) + " plugin(s)");

        for (auto* p : plugins)
        {
            ptl::diag::write ("[live]   plugin '" + p->getName() + "' enabled=" + juce::String (p->isEnabled() ? 1 : 0));

            if (! withParams) continue;

            int idx = 0;
            for (auto* ap : p->getAutomatableParameters())
                ptl::diag::write ("[live]     param[" + juce::String (idx++) + "] '" + ap->getParameterName()
                                  + "' = " + ap->getCurrentValueAsString()
                                  + "  (norm " + juce::String (ap->getCurrentNormalisedValue(), 3) + ")");
        }
    }

    // Find a parameter by NAME (index order differs between plugins / versions).
    te::AutomatableParameter::Ptr findParam (te::Plugin& p,
                                             std::initializer_list<const char*> exactNames,
                                             std::initializer_list<const char*> partialNames)
    {
        auto params = p.getAutomatableParameters();

        for (auto* want : exactNames)
            for (auto* ap : params)
                if (normaliseName (ap->getParameterName()) == want)
                    return te::AutomatableParameter::Ptr (ap);

        for (auto* want : partialNames)
            for (auto* ap : params)
                if (normaliseName (ap->getParameterName()).contains (want))
                    return te::AutomatableParameter::Ptr (ap);

        return {};
    }

    // External plugin parameters are always normalised 0..1: scan, read the text the plugin shows, pick the match.
    bool setParamToText (te::AutomatableParameter& ap, const juce::StringArray& wanted)
    {
        constexpr int steps = 400;

        for (int pass = 0; pass < 2; ++pass)          // pass 0: equal, pass 1: starts with
            for (int i = 0; i <= steps; ++i)
            {
                const float v = (float) i / (float) steps;
                const auto txt = plainText (ap.valueToString (v));

                for (auto& w : wanted)
                {
                    const auto target = plainText (w);
                    if (pass == 0 ? (txt == target) : txt.startsWith (target))
                    {
                        ap.setNormalisedParameter (v, juce::sendNotification);
                        return true;
                    }
                }
            }
        return false;
    }

    // Same for a number (semitones): the value whose displayed number is closest (error <= 0.5).
    bool setParamToNumber (te::AutomatableParameter& ap, double target)
    {
        constexpr int steps = 400;
        float bestV = -1.0f;
        double bestDiff = 1.0e9;

        for (int i = 0; i <= steps; ++i)
        {
            const float v = (float) i / (float) steps;
            const auto txt = ap.valueToString (v);
            if (! txt.containsAnyOf ("0123456789")) continue;

            const double diff = std::abs (txt.getDoubleValue() - target);
            if (diff < bestDiff) { bestDiff = diff; bestV = v; }
        }

        if (bestV < 0.0f || bestDiff > 0.5) return false;
        ap.setNormalisedParameter (bestV, juce::sendNotification);
        return true;
    }

    juce::StringArray rootNames (int semitone)
    {
        juce::StringArray out;
        out.addIfNotAlreadyThere (LiveController::semitoneName (semitone, false));
        out.addIfNotAlreadyThere (LiveController::semitoneName (semitone, true));
        return out;
    }

    int wrap12 (int v) { return ((v % 12) + 12) % 12; }
}

//==============================================================================
LiveController::LiveController (TrackManager& tm) : tracks (tm) {}

int LiveController::rootSemitone (const juce::String& root)
{
    static const std::map<juce::String, int> m = {
        {"C", 0}, {"C#", 1}, {"Db", 1}, {"D", 2}, {"D#", 3}, {"Eb", 3},
        {"E", 4}, {"F", 5}, {"F#", 6}, {"Gb", 6}, {"G", 7}, {"G#", 8},
        {"Ab", 8}, {"A", 9}, {"A#", 10}, {"Bb", 10}, {"B", 11}
    };
    const auto t = root.trim();
    if (t.isEmpty()) return -1;
    auto it = m.find (t.substring (0, 1).toUpperCase() + t.substring (1).toLowerCase());   // "db" -> "Db"
    return it == m.end() ? -1 : it->second;
}

juce::String LiveController::semitoneName (int semitone, bool preferFlats)
{
    static const char* sharps[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    static const char* flats[]  = { "C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B" };
    return (preferFlats ? flats : sharps)[wrap12 (semitone)];
}

juce::String LiveController::displayRootName (int semitone, bool minor)
{
    static const char* major[] = { "C","Db","D","Eb","E","F","F#","G","Ab","A","Bb","B" };
    static const char* mnr[]   = { "C","C#","D","Eb","E","F","F#","G","G#","A","Bb","B" };
    return (minor ? mnr : major)[wrap12 (semitone)];
}

juce::String LiveController::displayRootName (const juce::String& root, const juce::String& scale)
{
    const int s = rootSemitone (root);
    if (s < 0) return root;
    return displayRootName (s, scale.trim().equalsIgnoreCase ("Minor"));
}

juce::String LiveController::vietnameseKeyName (const juce::String& root, const juce::String& scale)
{
    const auto r = root.trim();
    if (r.isEmpty() || rootSemitone (r) < 0) return {};

    juce::String note;
    switch (r[0])
    {
        case 'C': note = juce::String::fromUTF8 ("ĐÔ"); break;
        case 'D': note = juce::String::fromUTF8 ("RÊ"); break;
        case 'E': note = "MI";  break;
        case 'F': note = "FA";  break;
        case 'G': note = "SOL"; break;
        case 'A': note = "LA";  break;
        case 'B': note = "SI";  break;
        default:  return {};
    }

    if (r.length() > 1)
    {
        if (r[1] == '#') note << juce::String::fromUTF8 (" THĂNG");
        else             note << juce::String::fromUTF8 (" GIÁNG");
    }

    note << (scale.trim().equalsIgnoreCase ("Minor") ? juce::String::fromUTF8 (" THỨ")
                                                     : juce::String::fromUTF8 (" TRƯỞNG"));
    return note;
}

te::AudioTrack* LiveController::findTrack (const juce::String& name) const
{
    for (auto* t : tracks.getAllTracks())
        if (t != nullptr && t->getName().equalsIgnoreCase (name))
            return t;
    return nullptr;
}

// Looks at EVERY track (any name), not only BEAT / MICRO: the Autotune and the SoundShifter may sit anywhere.
int LiveController::forEachPluginNamed (const juce::String& needle,
                                        const std::function<void (te::AudioTrack&, te::Plugin&)>& fn) const
{
    int count = 0;
    for (auto* t : tracks.getAllTracks())
    {
        if (t == nullptr) continue;
        for (auto* p : t->pluginList.getPlugins())
        {
            if (p == nullptr || ! pluginNameContains (*p, needle)) continue;
            ++count;
            fn (*t, *p);
        }
    }
    return count;
}

void LiveController::logAllTracksPlugins (const juce::String& why) const
{
    for (auto* t : tracks.getAllTracks())
        if (t != nullptr)
            logTrackPlugins (*t, why, false);
}

//== Toggles ===================================================================
bool LiveController::isBeatOn() const { auto* t = findTrack ("BEAT");  return t != nullptr && ! t->isMuted (false); }
bool LiveController::isMicOn()  const { auto* t = findTrack ("MICRO"); return t != nullptr && ! t->isMuted (false); }

bool LiveController::hasVang() const
{
    if (auto* mic = findTrack ("MICRO"))
        for (auto* s : tracks.getSends (mic))
            if (s->busNumber.get() != TrackManager::kMasterSendBus)
                return true;
    return false;
}

bool LiveController::isVangOn() const
{
    if (auto* mic = findTrack ("MICRO"))
        for (auto* s : tracks.getSends (mic))
            if (s->busNumber.get() != TrackManager::kMasterSendBus && s->isEnabled())
                return true;
    return false;
}

void LiveController::setBeatOn (bool on) { if (auto* t = findTrack ("BEAT"))  t->setMute (! on); changed(); }
void LiveController::setMicOn  (bool on) { if (auto* t = findTrack ("MICRO")) t->setMute (! on); changed(); }

void LiveController::setVangOn (bool on)
{
    if (auto* mic = findTrack ("MICRO"))
        for (auto* s : tracks.getSends (mic))
            if (s->busNumber.get() != TrackManager::kMasterSendBus)      // never touch the Master Send
                s->setEnabled (on);
    changed();
}

//== Levels ====================================================================
bool LiveController::hasVolume (Channel c) const
{
    return tracks.getVolumePlugin (trackFor (c)) != nullptr;
}

float LiveController::getVolumeDb (Channel c) const
{
    if (auto* vp = tracks.getVolumePlugin (trackFor (c)))
        return vp->getVolumeDb();
    return 0.0f;
}

void LiveController::setVolumeDb (Channel c, float db)
{
    if (auto* vp = tracks.getVolumePlugin (trackFor (c)))
        vp->setVolumeDb (db);
}

//== Auto-Tune knobs ===========================================================
te::AutomatableParameter::Ptr LiveController::findAutotuneKnob (AutotuneKnob k) const
{
    te::AutomatableParameter::Ptr result;

    // The first Autotune (on any track) that has the parameter wins.
    forEachPluginNamed ("autotune", [&] (te::AudioTrack& track, te::Plugin& p)
    {
        if (result != nullptr) return;

        auto param = (k == AutotuneKnob::RetuneSpeed) ? findParam (p, { "retunespeed" }, { "retune" })
                                                      : findParam (p, { "flextune", "flex" }, { "flex" });
        if (param != nullptr)
        {
            result = param;
            return;
        }

        const int idx = (int) k;
        if (! loggedKnobMissing[idx])
        {
            loggedKnobMissing[idx] = true;
            logTrackPlugins (track, juce::String (k == AutotuneKnob::RetuneSpeed ? "Retune Speed" : "Flex Tune")
                                    + " parameter not found in Autotune", true);
        }
    });

    return result;
}

float LiveController::getAutotuneKnob (AutotuneKnob k) const
{
    if (auto param = findAutotuneKnob (k))
        return param->getCurrentNormalisedValue();
    return -1.0f;
}

void LiveController::setAutotuneKnob (AutotuneKnob k, float norm01)
{
    if (auto param = findAutotuneKnob (k))
        param->setNormalisedParameter (juce::jlimit (0.0f, 1.0f, norm01), juce::sendNotification);
}

juce::String LiveController::getAutotuneKnobText (AutotuneKnob k, float norm01) const
{
    if (auto param = findAutotuneKnob (k))
        return param->valueToString (juce::jlimit (0.0f, 1.0f, norm01));
    return {};
}

//== User-made controls (sliders + toggles) ===================================
namespace
{
    using Item = LiveController::CtlItem;

    const juce::Identifier kItemsProp         { "ptlControlItems" };      // JSON text on the Edit
    const juce::Identifier kInsertsBypassKey  { "ptlInsertsBypass" };     // same properties the inspector's bypass buttons use
    const juce::Identifier kSendsBypassKey    { "ptlSendsBypass" };
    const juce::Identifier kWasOnKey          { "ptlWasOn" };

    // Old storage (TOGGLE 1..4 and the FX send slider lived on MICRO). Only read, to convert old sessions.
    const juce::Identifier kOldToggleId[4]      { "ptlRemix1", "ptlRemix2", "ptlRemix3", "ptlRemix4" };
    const juce::Identifier kOldToggleNameId[4]  { "ptlToggleName1", "ptlToggleName2", "ptlToggleName3", "ptlToggleName4" };
    const juce::Identifier kOldToggleShownId[4] { "ptlToggleShown1", "ptlToggleShown2", "ptlToggleShown3", "ptlToggleShown4" };
    const juce::Identifier kOldFxSliderBusId    { "ptlFxSliderBus" };

    constexpr float  kSendMinDb = -60.0f, kSendMaxDb = 6.02f;      // same range as the inspector's send slider
    constexpr double kSendDefaultDb = -16.0;

    double sendDbToPos (double db)  { return juce::jlimit (0.0, 1.0, (db - kSendMinDb) / (double) (kSendMaxDb - kSendMinDb)); }
    double sendPosToDb (double pos) { return std::round ((kSendMinDb + juce::jlimit (0.0, 1.0, pos) * (kSendMaxDb - kSendMinDb)) * 10.0) / 10.0; }

    juce::String sendDbText (double db)
    {
        return juce::String (db > 0.05 ? "+" : "") + juce::String (db, 1) + " dB";
    }

    // Track fader: position 0..1 <-> dB, same curve as the BEAT / MICRO sliders (0 dB at 80 %, +6 dB at the end, 0 = off).
    constexpr double kVolUnityPos = 0.8;

    double volPosToDb (double p)
    {
        if (p < 0.01) return -100.0;
        if (p <= kVolUnityPos) return juce::jmap (p, 0.01, kVolUnityPos, -50.0, 0.0);
        return juce::jmap (p, kVolUnityPos, 1.0, 0.0, 6.0);
    }

    double volDbToPos (double db)
    {
        if (db <= -99.0) return 0.0;
        if (db <= 0.0)   return juce::jmap (juce::jmax (db, -50.0), -50.0, 0.0, 0.01, kVolUnityPos);
        return juce::jmap (juce::jmin (db, 6.0), 0.0, 6.0, kVolUnityPos, 1.0);
    }

    juce::String volText (double db)
    {
        if (db <= -99.0) return "-inf";
        return juce::String (db > 0.05 ? "+" : "") + juce::String (db, 1) + " dB";
    }

    // Pan: position 0..1 <-> -1..+1 (centre = 0.5), same text as the inspector knob.
    double panPosToValue (double p) { return std::round ((juce::jlimit (0.0, 1.0, p) * 2.0 - 1.0) * 100.0) / 100.0; }
    double panValueToPos (double v) { return juce::jlimit (0.0, 1.0, (v + 1.0) * 0.5); }

    juce::String panText (double v)
    {
        if (std::abs (v) < 0.005) return "C";
        return (v < 0 ? juce::String ("L ") : juce::String ("R ")) + juce::String (juce::roundToInt (std::abs (v) * 100));
    }

    const char* kindKey (Item::Kind k)
    {
        switch (k)
        {
            case Item::Kind::ParamSlider:  return "param";
            case Item::Kind::SendSlider:   return "send";
            case Item::Kind::VolumeSlider: return "vol";
            case Item::Kind::PanSlider:    return "pan";
            case Item::Kind::EmptySlider:  return "empty";
            case Item::Kind::Toggle:       break;
        }
        return "toggle";
    }

    const char* fnKey (Item::Fn f)
    {
        switch (f)
        {
            case Item::Fn::TrackOn:   return "trackOn";
            case Item::Fn::TrackSolo: return "trackSolo";
            case Item::Fn::PluginOn:  return "pluginOn";
            case Item::Fn::InsertsOn: return "insertsOn";
            case Item::Fn::SendsOn:   return "sendsOn";
            case Item::Fn::SendOn:    return "sendOn";
            case Item::Fn::Pdc:       return "pdc";
            case Item::Fn::AllEffects: return "allFx";
            case Item::Fn::None:      return "none";
        }
        return "pluginOn";
    }

    Item::Fn fnFromKey (const juce::String& k)
    {
        for (auto f : { Item::Fn::TrackOn, Item::Fn::TrackSolo, Item::Fn::PluginOn, Item::Fn::InsertsOn,
                        Item::Fn::SendsOn, Item::Fn::SendOn, Item::Fn::Pdc, Item::Fn::AllEffects, Item::Fn::None })
            if (k == fnKey (f)) return f;
        return Item::Fn::PluginOn;
    }

    juce::String itemsToJson (const std::vector<Item>& v)
    {
        juce::Array<juce::var> arr;
        for (auto& it : v)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id", it.id);
            o->setProperty ("k", juce::String (kindKey (it.kind)));
            if (it.kind == Item::Kind::Toggle)  o->setProperty ("fn", juce::String (fnKey (it.fn)));
            if (it.name.isNotEmpty())           o->setProperty ("name", it.name);
            o->setProperty ("track", it.trackId);
            if (it.pluginId.isNotEmpty())       o->setProperty ("plugin", it.pluginId);
            if (it.paramId.isNotEmpty())        o->setProperty ("param", it.paramId);
            if (it.paramName.isNotEmpty())      o->setProperty ("pname", it.paramName);
            if (it.bus >= 0)                    o->setProperty ("bus", it.bus);
            arr.add (juce::var (o));
        }
        return juce::JSON::toString (juce::var (arr), true);
    }

    std::vector<Item> itemsFromJson (const juce::String& json)
    {
        std::vector<Item> out;
        const auto root = juce::JSON::parse (json);
        auto* arr = root.getArray();
        if (arr == nullptr) return out;

        for (auto& v : *arr)
        {
            if (! v.isObject()) continue;
            Item it;
            it.id        = (int) v.getProperty ("id", 0);
            const auto k = v.getProperty ("k", "toggle").toString();
            it.kind      = k == "param" ? Item::Kind::ParamSlider
                         : k == "send"  ? Item::Kind::SendSlider
                         : k == "vol"   ? Item::Kind::VolumeSlider
                         : k == "pan"   ? Item::Kind::PanSlider
                         : k == "empty" ? Item::Kind::EmptySlider : Item::Kind::Toggle;
            it.fn        = fnFromKey (v.getProperty ("fn", "pluginOn").toString());
            it.name      = v.getProperty ("name", "").toString();
            it.trackId   = v.getProperty ("track", "").toString();
            it.pluginId  = v.getProperty ("plugin", "").toString();
            it.paramId   = v.getProperty ("param", "").toString();
            it.paramName = v.getProperty ("pname", "").toString();
            it.bus       = (int) v.getProperty ("bus", -1);
            if (it.id > 0) out.push_back (it);
        }
        return out;
    }

    bool insertsBypassed (te::Track& t) { return static_cast<bool> (t.state.getProperty (kInsertsBypassKey, false)); }
    bool sendsBypassed   (te::Track& t) { return static_cast<bool> (t.state.getProperty (kSendsBypassKey, false)); }
}

//-- storage ---------------------------------------------------------------------
void LiveController::loadItems() const
{
    const auto& st = tracks.getEdit().state;

    if (st.hasProperty (kItemsProp))
    {
        const auto json = st.getProperty (kItemsProp).toString();
        if (itemsLoaded && json == itemsJson) return;          // nothing changed since the last read

        items = itemsFromJson (json);
        itemsJson = json;
        itemsLoaded = true;
        return;
    }

    // No property yet: a session made before this feature. Convert the old MICRO properties once.
    items.clear();
    itemsJson = {};
    itemsLoaded = true;

    auto* mic = findTrack ("MICRO");
    if (mic == nullptr) { itemsLoaded = false; return; }       // tracks not created yet: try again later (nothing is stored)

    int nextId = 1;
    for (int i = 0; i < 4; ++i)
    {
        const bool shown = static_cast<bool> (mic->state.getProperty (kOldToggleShownId[i], false));
        const auto pid   = mic->state.getProperty (kOldToggleId[i]).toString();
        const auto nm    = mic->state.getProperty (kOldToggleNameId[i]).toString().trim();
        if (! shown && pid.isEmpty() && nm.isEmpty()) continue;

        Item it;
        it.id = nextId++;
        it.kind = Item::Kind::Toggle;
        it.fn = Item::Fn::PluginOn;
        it.name = nm;
        it.trackId = mic->itemID.toString();
        it.pluginId = pid;
        items.push_back (it);
    }

    if (mic->state.hasProperty (kOldFxSliderBusId))
    {
        Item it;
        it.id = nextId++;
        it.kind = Item::Kind::SendSlider;
        it.trackId = mic->itemID.toString();
        it.bus = (int) mic->state.getProperty (kOldFxSliderBusId);
        items.push_back (it);
    }

    if (items.empty())
        seedPlaceholders();              // brand-new session: 2 empty toggles + 4 empty sliders for the user to assign
    else
        saveItems();
}

// New session: 2 empty TOGGLEs and 4 empty SLIDERs (nothing assigned). The user assigns a function with a click
// (or right-click > "Gán chức năng") and can delete any of them. Stored in the session like every other control.
void LiveController::seedPlaceholders() const
{
    items.clear();
    int id = 1;
    for (int i = 0; i < 2; ++i)
    {
        Item it;
        it.id = id++;
        it.kind = Item::Kind::Toggle;
        it.fn = Item::Fn::None;
        items.push_back (it);
    }
    for (int i = 0; i < 4; ++i)
    {
        Item it;
        it.id = id++;
        it.kind = Item::Kind::EmptySlider;
        items.push_back (it);
    }
    saveItems();
}

void LiveController::saveItems() const
{
    itemsJson = itemsToJson (items);
    itemsLoaded = true;
    tracks.getEdit().state.setProperty (kItemsProp, itemsJson, nullptr);
}

std::vector<LiveController::CtlItem> LiveController::getItems() const
{
    loadItems();
    return items;
}

const LiveController::CtlItem* LiveController::findItem (int id) const
{
    loadItems();
    for (auto& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

int LiveController::addItem (CtlItem item)
{
    loadItems();
    int maxId = 0;
    for (auto& it : items) maxId = juce::jmax (maxId, it.id);
    item.id = maxId + 1;
    items.push_back (item);
    saveItems();
    ptl::diag::write ("[live] control #" + juce::String (item.id) + " (" + kindKey (item.kind) + ") added");
    changed();
    return item.id;
}

bool LiveController::hasToggle (CtlItem::Fn fn, const juce::String& trackId, const juce::String& pluginId, int bus) const
{
    loadItems();
    for (auto& it : items)
        if (it.kind == Item::Kind::Toggle && it.fn == fn && it.trackId == trackId
            && it.pluginId == pluginId && it.bus == bus)
            return true;
    return false;
}

int LiveController::addParamSlider (const juce::String& trackId, const juce::String& pluginId,
                                    const juce::String& paramId, const juce::String& paramName)
{
    Item it;
    it.kind = Item::Kind::ParamSlider;
    it.trackId = trackId;
    it.pluginId = pluginId;
    it.paramId = paramId;
    it.paramName = paramName;
    return addItem (it);
}

int LiveController::addSendSlider (const juce::String& trackId, int bus)
{
    Item it;
    it.kind = Item::Kind::SendSlider;
    it.trackId = trackId;
    it.bus = bus;
    return addItem (it);
}

bool LiveController::hasTrackSlider (CtlItem::Kind kind, const juce::String& trackId) const
{
    loadItems();
    for (auto& it : items)
        if (it.kind == kind && it.trackId == trackId)
            return true;
    return false;
}

int LiveController::addTrackSlider (CtlItem::Kind kind, const juce::String& trackId)
{
    if (kind != Item::Kind::VolumeSlider && kind != Item::Kind::PanSlider)
        return -1;
    if (hasTrackSlider (kind, trackId))               // one volume / one pan slider per track
        return -1;

    Item it;
    it.kind = kind;
    it.trackId = trackId;
    return addItem (it);
}

int LiveController::addToggle (CtlItem::Fn fn, const juce::String& trackId, const juce::String& pluginId, int bus)
{
    if (hasToggle (fn, trackId, pluginId, bus))      // one function = one button
        return -1;

    Item it;
    it.kind = Item::Kind::Toggle;
    it.fn = fn;
    it.trackId = trackId;
    it.pluginId = pluginId;
    it.bus = bus;
    return addItem (it);
}

void LiveController::removeItem (int id)
{
    loadItems();
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id)
        {
            items.erase (items.begin() + (std::ptrdiff_t) i);
            saveItems();
            ptl::diag::write ("[live] control #" + juce::String (id) + " removed");
            changed();
            return;
        }
}

void LiveController::renameItem (int id, const juce::String& newName)
{
    loadItems();
    for (auto& it : items)
        if (it.id == id)
        {
            it.name = newName.trim();
            saveItems();
            changed();
            return;
        }
}

//-- finding the targets ----------------------------------------------------------
te::Track* LiveController::findTrackById (const juce::String& id) const
{
    if (id.isEmpty()) return nullptr;

    for (auto* t : tracks.getAllTracks())
        if (t != nullptr && t->itemID.toString() == id)
            return t;

    if (auto* m = tracks.getMasterTrack())
        if (m->itemID.toString() == id)
            return m;

    return nullptr;
}

te::Plugin* LiveController::findPluginById (te::Track& t, const juce::String& id) const
{
    if (id.isEmpty()) return nullptr;

    for (auto* p : tracks.getPluginList (&t).getPlugins())
        if (dynamic_cast<te::ExternalPlugin*> (p) != nullptr && p->itemID.toString() == id)
            return p;
    return nullptr;
}

te::AuxSendPlugin* LiveController::findSendOf (te::Track& t, int bus) const
{
    auto* at = dynamic_cast<te::AudioTrack*> (&t);
    if (at == nullptr) return nullptr;

    for (auto* s : tracks.getSends (at))
        if (s != nullptr && s->busNumber.get() == bus)
            return s;
    return nullptr;
}

te::AutomatableParameter::Ptr LiveController::findParamOf (const CtlItem& it) const
{
    auto* t = findTrackById (it.trackId);
    if (t == nullptr) return {};

    auto* p = findPluginById (*t, it.pluginId);
    if (p == nullptr) return {};

    auto params = p->getAutomatableParameters();

    for (auto* ap : params)
        if (ap != nullptr && ap->paramID == it.paramId)
            return te::AutomatableParameter::Ptr (ap);

    if (it.paramName.isNotEmpty())                       // id changed (plugin update): fall back to the name
        for (auto* ap : params)
            if (ap != nullptr && ap->getParameterName() == it.paramName)
                return te::AutomatableParameter::Ptr (ap);

    return {};
}

//-- names -------------------------------------------------------------------------
juce::String LiveController::defaultNameOf (const CtlItem& it) const
{
    auto* t = findTrackById (it.trackId);
    const auto trackName = t != nullptr ? t->getName() : juce::String ("?");

    switch (it.kind)
    {
        case Item::Kind::ParamSlider:
        {
            if (auto ap = findParamOf (it)) return ap->getParameterName();
            return it.paramName.isNotEmpty() ? it.paramName : juce::String ("?");
        }

        case Item::Kind::SendSlider:
        {
            auto* s = t != nullptr ? findSendOf (*t, it.bus) : nullptr;
            const auto target = s != nullptr ? tracks.getSendTargetName (s) : juce::String ("FX");
            return trackName.equalsIgnoreCase ("MICRO") ? target : trackName + " > " + target;
        }

        case Item::Kind::VolumeSlider: return "VOL " + trackName;
        case Item::Kind::PanSlider:    return "PAN " + trackName;
        case Item::Kind::EmptySlider:  return "SLIDER";

        case Item::Kind::Toggle:
            break;
    }

    switch (it.fn)
    {
        case Item::Fn::TrackOn:   return trackName;
        case Item::Fn::TrackSolo: return "SOLO " + trackName;
        case Item::Fn::InsertsOn: return "INS " + trackName;
        case Item::Fn::SendsOn:   return "SEND " + trackName;
        case Item::Fn::Pdc:       return "PDC";
        case Item::Fn::AllEffects: return "ALL EFFECT";
        case Item::Fn::None:      return "TOGGLE";
        case Item::Fn::PluginOn:
        {
            auto* p = t != nullptr ? findPluginById (*t, it.pluginId) : nullptr;
            return p != nullptr ? p->getName() : juce::String ("VST");
        }
        case Item::Fn::SendOn:
        {
            auto* s = t != nullptr ? findSendOf (*t, it.bus) : nullptr;
            return s != nullptr ? tracks.getSendTargetName (s) : juce::String ("SEND");
        }
    }
    return "?";
}

juce::String LiveController::getItemName (int id) const
{
    auto* it = findItem (id);
    if (it == nullptr) return {};
    return it->name.isNotEmpty() ? it->name : defaultNameOf (*it);
}

//-- sliders -----------------------------------------------------------------------
LiveController::SliderView LiveController::getSliderView (int id) const
{
    SliderView v;
    auto* it = findItem (id);
    if (it == nullptr) return v;

    v.label = it->name.isNotEmpty() ? it->name : defaultNameOf (*it);

    auto* t = findTrackById (it->trackId);
    const auto trackName = t != nullptr ? t->getName() : juce::String ("?");
    const auto help = juce::String::fromUTF8 ("\nKéo / lăn chuột: chỉnh ngay   |   Nhấp đúp vào tên: đổi tên   |   Chuột phải: đổi tên / xoá");

    if (it->kind == Item::Kind::EmptySlider)
    {
        v.unassigned = true;
        v.tip = juce::String::fromUTF8 ("Slider trống - chưa gán chức năng.\nBấm (hoặc chuột phải) > Gán chức năng...   |   Chuột phải > Xoá: bỏ slider này");
    }
    else if (it->kind == Item::Kind::ParamSlider)
    {
        auto* p = t != nullptr ? findPluginById (*t, it->pluginId) : nullptr;
        if (auto ap = findParamOf (*it))
        {
            v.available = true;
            v.pos = ap->getCurrentNormalisedValue();
            v.tip = trackName + " > " + (p != nullptr ? p->getName() : juce::String ("?")) + " > " + ap->getParameterName() + help;
        }
        else
            v.tip = trackName + " > " + (p != nullptr ? p->getName() : juce::String ("?")) + " > " + it->paramName
                    + juce::String::fromUTF8 ("\nKhông tìm thấy parameter này (VST / track đã bị xoá hoặc chưa nạp). Chuột phải: xoá slider");
    }
    else if (it->kind == Item::Kind::SendSlider)
    {
        auto* s = t != nullptr ? findSendOf (*t, it->bus) : nullptr;
        if (s != nullptr)
        {
            v.available = true;
            v.pos = sendDbToPos (s->getGainDb());
            v.tip = juce::String::fromUTF8 ("Mức send ") + trackName + " > " + tracks.getSendTargetName (s) + help;
        }
        else
            v.tip = juce::String::fromUTF8 ("Send này không còn nữa. Chuột phải: xoá slider");
    }
    else if (it->kind == Item::Kind::VolumeSlider || it->kind == Item::Kind::PanSlider)
    {
        const bool isVol = it->kind == Item::Kind::VolumeSlider;
        if (auto* vp = tracks.getVolumePlugin (t))
        {
            v.available = true;
            v.pos = isVol ? volDbToPos (vp->getVolumeDb()) : panValueToPos (vp->getPan());
            v.tip = juce::String (isVol ? "Volume " : "Pan ") + trackName + help;
        }
        else
            v.tip = juce::String::fromUTF8 ("Track này không còn nữa. Chuột phải: xoá slider");
    }
    return v;
}

juce::String LiveController::getSliderText (int id, double pos) const
{
    auto* it = findItem (id);
    if (it == nullptr) return {};

    if (it->kind == Item::Kind::ParamSlider)
    {
        if (auto ap = findParamOf (*it))
            return ap->valueToString ((float) juce::jlimit (0.0, 1.0, pos));      // the VST's own text ("20", "C#", "35 %" ...)
        return {};
    }

    if (it->kind == Item::Kind::EmptySlider)  return {};
    if (it->kind == Item::Kind::VolumeSlider) return volText (volPosToDb (pos));
    if (it->kind == Item::Kind::PanSlider)    return panText (panPosToValue (pos));

    return sendDbText (sendPosToDb (pos));
}

void LiveController::setSliderPos (int id, double pos)
{
    auto* it = findItem (id);
    if (it == nullptr) return;

    if (it->kind == Item::Kind::ParamSlider)
    {
        if (auto ap = findParamOf (*it))
            ap->setNormalisedParameter ((float) juce::jlimit (0.0, 1.0, pos), juce::sendNotification);
    }
    else if (it->kind == Item::Kind::SendSlider)
    {
        if (auto* t = findTrackById (it->trackId))
            if (auto* s = findSendOf (*t, it->bus))
                s->setGainDb (juce::jlimit (kSendMinDb, kSendMaxDb, (float) sendPosToDb (pos)));
    }
    else if (it->kind == Item::Kind::VolumeSlider || it->kind == Item::Kind::PanSlider)
    {
        if (auto* vp = tracks.getVolumePlugin (findTrackById (it->trackId)))
        {
            if (it->kind == Item::Kind::VolumeSlider) vp->setVolumeDb ((float) volPosToDb (pos));
            else                                      vp->setPan ((float) panPosToValue (pos));
        }
    }
}

double LiveController::getSliderDefaultPos (int id) const
{
    auto* it = findItem (id);
    if (it == nullptr) return -1.0;
    if (it->kind == Item::Kind::SendSlider)   return sendDbToPos (kSendDefaultDb);
    if (it->kind == Item::Kind::VolumeSlider) return volDbToPos (0.0);          // double-click = 0 dB
    if (it->kind == Item::Kind::PanSlider)    return 0.5;                       // double-click = centre
    return -1.0;
}

//-- toggles -----------------------------------------------------------------------
LiveController::ToggleView LiveController::getToggleView (int id) const
{
    ToggleView v;
    auto* it = findItem (id);
    if (it == nullptr || it->kind != Item::Kind::Toggle) return v;

    v.label = it->name.isNotEmpty() ? it->name : defaultNameOf (*it);

    auto* t  = findTrackById (it->trackId);
    auto* at = dynamic_cast<te::AudioTrack*> (t);
    const auto tn = t != nullptr ? t->getName() : juce::String ("?");
    juce::String what;

    switch (it->fn)
    {
        case Item::Fn::None:
            v.unassigned = true;
            v.tip = juce::String::fromUTF8 ("Toggle trống - chưa gán chức năng.\nBấm (hoặc chuột phải) > Gán chức năng...   |   Chuột phải > Xoá: bỏ nút này");
            return v;

        case Item::Fn::AllEffects:
            what = juce::String::fromUTF8 ("Bật / tắt TOÀN BỘ hiệu ứng: INSERT + SEND của mọi track, kể cả Master (tắt = âm vào = âm ra)");
            v.available = true;
            v.on = allEffectsActive();
            break;

        case Item::Fn::TrackOn:
            what = juce::String::fromUTF8 ("Bật / tắt tiếng track ") + tn;
            if (at != nullptr) { v.available = true; v.on = ! at->isMuted (false); }
            break;

        case Item::Fn::TrackSolo:
            what = juce::String::fromUTF8 ("Solo track ") + tn;
            if (at != nullptr) { v.available = true; v.on = at->isSolo (false); }
            break;

        case Item::Fn::PluginOn:
        {
            auto* p = t != nullptr ? findPluginById (*t, it->pluginId) : nullptr;
            what = juce::String::fromUTF8 ("Bật / tắt VST ") + (p != nullptr ? p->getName() : juce::String ("?")) + " (" + tn + ")";
            if (p != nullptr)
            {
                v.available = true;
                v.locked = insertsBypassed (*t);
                v.on = p->isEnabled();
            }
            break;
        }

        case Item::Fn::InsertsOn:
            what = juce::String::fromUTF8 ("Bật / tắt TẤT CẢ INSERT của ") + tn;
            if (t != nullptr) { v.available = true; v.on = ! insertsBypassed (*t); }
            break;

        case Item::Fn::SendsOn:
            what = juce::String::fromUTF8 ("Bật / tắt TẤT CẢ SEND của ") + tn;
            if (at != nullptr) { v.available = true; v.on = ! sendsBypassed (*at); }
            break;

        case Item::Fn::SendOn:
        {
            auto* s = t != nullptr ? findSendOf (*t, it->bus) : nullptr;
            what = juce::String::fromUTF8 ("Bật / tắt send ") + tn + " > " + (s != nullptr ? tracks.getSendTargetName (s) : juce::String ("?"));
            if (s != nullptr)
            {
                v.available = true;
                v.locked = sendsBypassed (*t);
                v.on = s->isEnabled();
            }
            break;
        }

        case Item::Fn::Pdc:
            what = juce::String::fromUTF8 ("Bật / tắt PDC (bù trễ plugin)");
            if (isPdcOn) { v.available = true; v.on = isPdcOn(); }
            break;
    }

    if (! v.available)
        v.tip = what + juce::String::fromUTF8 ("\nKhông tìm thấy mục tiêu (track / VST / send đã bị xoá). Chuột phải: xoá nút này");
    else if (v.locked)
        v.tip = what + juce::String::fromUTF8 ("\nĐang bị bypass cả nhóm trong Inspector (bỏ bypass ở đó để dùng nút này)");
    else
        v.tip = what + juce::String::fromUTF8 ("\nBấm: bật / tắt   |   Nhấp đúp: đổi tên   |   Chuột phải: đổi tên / xoá");

    return v;
}

void LiveController::setToggleOn (int id, bool on)
{
    auto* it = findItem (id);
    if (it == nullptr || it->kind != Item::Kind::Toggle) return;

    auto* t  = findTrackById (it->trackId);
    auto* at = dynamic_cast<te::AudioTrack*> (t);

    switch (it->fn)
    {
        case Item::Fn::TrackOn:   if (at != nullptr) at->setMute (! on); break;
        case Item::Fn::TrackSolo: if (at != nullptr) at->setSolo (on);   break;

        case Item::Fn::PluginOn:
            if (t != nullptr && ! insertsBypassed (*t))                  // a bypassed INSERTS section keeps its plugins off
                if (auto* p = findPluginById (*t, it->pluginId))
                    p->setEnabled (on);
            break;

        case Item::Fn::InsertsOn: if (t != nullptr)  setInsertsBypassed (*t, ! on); break;
        case Item::Fn::SendsOn:   if (at != nullptr) setSendsBypassed (*at, ! on);  break;

        case Item::Fn::SendOn:
            if (t != nullptr && ! sendsBypassed (*t))
                if (auto* s = findSendOf (*t, it->bus))
                    s->setEnabled (on);
            break;

        case Item::Fn::Pdc:       if (setPdcOn) setPdcOn (on); break;
        case Item::Fn::AllEffects: setAllEffectsBypassed (! on); break;
        case Item::Fn::None:      return;
    }

    ptl::diag::write ("[live] toggle '" + getItemName (id) + "' -> " + (on ? "ON" : "OFF"));
    changed();
}

// Same behaviour as the inspector's INSERTS / SENDS bypass buttons (the "ptlWasOn" marker remembers what
// was on before the bypass, so un-bypass puts back exactly that).
void LiveController::setInsertsBypassed (te::Track& track, bool bypass)
{
    if (bypass) track.state.setProperty (kInsertsBypassKey, true, nullptr);
    else        track.state.removeProperty (kInsertsBypassKey, nullptr);

    for (auto* p : tracks.getPluginList (&track).getPlugins())
    {
        if (dynamic_cast<te::ExternalPlugin*> (p) == nullptr) continue;

        if (bypass)
        {
            if (p->isEnabled())
            {
                p->state.setProperty (kWasOnKey, true, nullptr);
                p->setEnabled (false);
            }
        }
        else if (static_cast<bool> (p->state.getProperty (kWasOnKey, false)))
        {
            p->state.removeProperty (kWasOnKey, nullptr);
            p->setEnabled (true);
        }
    }
}

void LiveController::setSendsBypassed (te::Track& track, bool bypass)
{
    if (bypass) track.state.setProperty (kSendsBypassKey, true, nullptr);
    else        track.state.removeProperty (kSendsBypassKey, nullptr);

    auto* at = dynamic_cast<te::AudioTrack*> (&track);
    if (at == nullptr) return;

    for (auto* s : tracks.getSends (at))                   // FX sends AND the Master send, like the inspector
    {
        if (bypass)
        {
            if (s->isEnabled())
            {
                s->state.setProperty (kWasOnKey, true, nullptr);
                s->setEnabled (false);
            }
        }
        else if (static_cast<bool> (s->state.getProperty (kWasOnKey, false)))
        {
            s->state.removeProperty (kWasOnKey, nullptr);
            s->setEnabled (true);
        }
    }
}

// ALL EFFECT: the same bypass the inspector's INSERTS / SENDS buttons use, applied to EVERY track and the Master.
// OFF = no insert and no send runs anywhere (sound in = sound out); ON puts back exactly what was on before.
void LiveController::setAllEffectsBypassed (bool bypass)
{
    for (auto* t : tracks.getAllTracks())
        if (t != nullptr)
        {
            setInsertsBypassed (*t, bypass);
            setSendsBypassed   (*t, bypass);
        }

    if (auto* m = tracks.getMasterTrack())
        setInsertsBypassed (*m, bypass);            // the Master has inserts only

    ptl::diag::write (juce::String ("[live] ALL EFFECT -> ") + (bypass ? "OFF (dry)" : "ON"));
}

// Lit while at least one INSERTS / SENDS section is active; dark only when every one is bypassed.
bool LiveController::allEffectsActive() const
{
    for (auto* t : tracks.getAllTracks())
        if (t != nullptr && (! insertsBypassed (*t) || ! sendsBypassed (*t)))
            return true;

    if (auto* m = tracks.getMasterTrack())
        if (! insertsBypassed (*m))
            return true;

    return false;
}

bool LiveController::isUnassigned (int id) const
{
    auto* it = findItem (id);
    return it != nullptr && (it->kind == Item::Kind::EmptySlider
                             || (it->kind == Item::Kind::Toggle && it->fn == Item::Fn::None));
}

// Fills an item in place: same id, same place in the panel. A slider can only become a slider, a toggle a toggle.
bool LiveController::assignItem (int id, CtlItem target)
{
    loadItems();
    for (auto& it : items)
        if (it.id == id)
        {
            const bool wasToggle = it.kind == Item::Kind::Toggle;
            const bool toToggle  = target.kind == Item::Kind::Toggle;
            if (wasToggle != toToggle) return false;

            target.id = id;
            target.name = {};                         // the old name belonged to the old function
            it = target;
            saveItems();
            ptl::diag::write ("[live] control #" + juce::String (id) + " assigned (" + kindKey (it.kind) + ")");
            changed();
            return true;
        }
    return false;
}

juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> LiveController::getMasterLevelMeter() const
{
    auto& edit = tracks.getEdit();
    auto& masterPlugins = edit.getMasterPluginList();

    if (auto* existing = masterPlugins.findFirstPluginOfType<te::LevelMeterPlugin>())
        return existing;

    if (auto plugin = edit.getPluginCache().createNewPlugin (te::LevelMeterPlugin::xmlTypeName, {}))
    {
        masterPlugins.insertPlugin (plugin, -1, nullptr);          // END of the master chain = after the master fader
        return dynamic_cast<te::LevelMeterPlugin*> (plugin.get());
    }
    return {};
}

//-- what the "+" menus list --------------------------------------------------------
juce::Array<LiveController::TrackInfo> LiveController::getControlTracks() const
{
    juce::Array<TrackInfo> out;

    for (auto* t : tracks.getAllTracks())
        if (t != nullptr)
            out.add ({ t->itemID.toString(), t->getName(), false });

    if (auto* m = tracks.getMasterTrack())
        out.add ({ m->itemID.toString(), m->getName().isNotEmpty() ? m->getName() : juce::String ("MASTER"), true });

    return out;
}

juce::Array<LiveController::InsertInfo> LiveController::getTrackInserts (const juce::String& trackId) const
{
    juce::Array<InsertInfo> out;
    if (auto* t = findTrackById (trackId))
        for (auto* p : tracks.getPluginList (t).getPlugins())
            if (dynamic_cast<te::ExternalPlugin*> (p) != nullptr)
                out.add ({ p->itemID.toString(), p->getName() });
    return out;
}

juce::Array<LiveController::FxSendInfo> LiveController::getTrackSends (const juce::String& trackId) const
{
    juce::Array<FxSendInfo> out;
    if (auto* at = dynamic_cast<te::AudioTrack*> (findTrackById (trackId)))
        for (auto* s : tracks.getSends (at))
            if (s != nullptr)
                out.add ({ s->busNumber.get(), tracks.getSendTargetName (s) });
    return out;
}

// Every parameter the VST publishes to the host (some have hundreds: the panel shows them in a searchable list).
juce::Array<LiveController::ParamInfo> LiveController::getPluginParams (const juce::String& trackId, const juce::String& pluginId)
{
    juce::Array<ParamInfo> out;
    lastParamError = {};

    auto* t = findTrackById (trackId);
    auto* p = t != nullptr ? findPluginById (*t, pluginId) : nullptr;
    if (p == nullptr)
    {
        lastParamError = juce::String::fromUTF8 ("Không tìm thấy VST này nữa.");
        return out;
    }

    // A lazily loaded / bypassed VST has no parameters yet: load it, but leave it as off as it was.
    const bool wasOn = p->isEnabled();
    const auto problem = ptl::pluginhost::ensureLoaded (*p);
    if (! wasOn && p->isEnabled())
        p->setEnabled (false);

    if (problem.isNotEmpty())
    {
        lastParamError = problem;
        return out;
    }

    for (auto* ap : p->getAutomatableParameters())
        if (ap != nullptr)
            out.add ({ ap->paramID, ap->getParameterName(), ap->getCurrentValueAsString() });

    if (out.isEmpty())
        lastParamError = juce::String::fromUTF8 ("VST này không công bố parameter nào cho phần mềm host.");

    return out;
}

//== Tone / key ================================================================
void LiveController::stepTone (int delta)
{
    if (busy || delta == 0) return;
    toneSteps = juce::jlimit (-12, 12, toneSteps + delta);
    applyToneToPlugin();
    applyKeyToAutotune();
    changed();
}

void LiveController::stepKeyOffset (int delta)
{
    if (busy || delta == 0) return;
    keyOffset = juce::jlimit (-12, 12, keyOffset + delta);
    applyKeyToAutotune();
    changed();
}

void LiveController::setDetectedKey (const juce::String& root, const juce::String& scale)
{
    const int r = rootSemitone (root);
    if (r < 0)
    {
        ptl::diag::write ("[live] unknown root '" + root + "'");
        return;
    }

    baseRoot  = r;
    baseScale = scale.trim().isEmpty() ? juce::String ("Major")
                                       : scale.trim().substring (0, 1).toUpperCase() + scale.trim().substring (1).toLowerCase();

    applyKeyToAutotune();
    changed();
}

void LiveController::resetShifts()
{
    const bool hadTone = toneSteps != 0;
    toneSteps = 0;
    keyOffset = 0;
    if (hadTone)
        applyToneToPlugin();                 // the new beat must play at its original pitch
    applyKeyToAutotune();                    // old base key, offsets cleared
    changed();
}

void LiveController::setManualKey (int rootSemi, bool minor)
{
    setDetectedKey (semitoneName (rootSemi, ! minor), minor ? "Minor" : "Major");
}

juce::String LiveController::getFinalRootName() const
{
    if (baseRoot < 0) return {};
    // Fixed display list (see displayRootName): e.g. A# minor is shown as Bb minor.
    return displayRootName (baseRoot + toneSteps + keyOffset, ! baseScale.equalsIgnoreCase ("Major"));
}

void LiveController::applyToneToPlugin()
{
    const int found = forEachPluginNamed ("soundshifter pitch", [&] (te::AudioTrack& track, te::Plugin& p)
    {
        auto param = findParam (p, { "semitones", "semitone", "pitchsemit", "pitchsemitones" },
                                   { "semi", "pitch" });
        if (param == nullptr)
        {
            logTrackPlugins (track, "tone: no semitone-like parameter", true);
            return;
        }

        const bool ok = setParamToNumber (*param, toneSteps);
        ptl::diag::write ("[live] tone=" + juce::String (toneSteps) + " -> '" + p.getName() + "' on track '"
                          + track.getName() + "' param '" + param->getParameterName() + "' "
                          + (ok ? "OK, now shows '" : "FAILED, still shows '")
                          + param->getCurrentValueAsString() + "'");
        if (! ok)
            logTrackPlugins (track, "tone: could not map value", true);
    });

    if (found == 0)
        logAllTracksPlugins ("tone: no plugin matching 'soundshifter pitch' on any track");
}

void LiveController::applyKeyToAutotune()
{
    if (baseRoot < 0) return;

    const int semitone = wrap12 (baseRoot + toneSteps + keyOffset);

    const int found = forEachPluginNamed ("autotune", [&] (te::AudioTrack& track, te::Plugin& p)
    {
        auto keyP   = findParam (p, { "key", "root", "tonic" }, { "key" });
        auto scaleP = findParam (p, { "scale" }, { "scale" });

        const bool keyOk   = keyP != nullptr   && setParamToText (*keyP, rootNames (semitone));
        const bool scaleOk = scaleP != nullptr && setParamToText (*scaleP, { baseScale });

        ptl::diag::write ("[live] key=" + semitoneName (semitone, false) + " scale=" + baseScale + " -> '" + p.getName()
                          + "' on track '" + track.getName()
                          + "' | key " + (keyP != nullptr ? (keyOk ? "OK=" : "FAILED=") + keyP->getCurrentValueAsString()
                                                          : juce::String ("NOT FOUND"))
                          + " | scale " + (scaleP != nullptr ? (scaleOk ? "OK=" : "FAILED=") + scaleP->getCurrentValueAsString()
                                                             : juce::String ("NOT FOUND")));
        if (! keyOk || ! scaleOk)
            logTrackPlugins (track, "key/scale: could not map", true);
    });

    if (found == 0)
        logAllTracksPlugins ("key: no plugin matching 'autotune' on any track");
}

void LiveController::dumpPlugins() const
{
    for (auto* t : tracks.getAllTracks())
        if (t != nullptr)
            logTrackPlugins (*t, "DUMP", true);
}
