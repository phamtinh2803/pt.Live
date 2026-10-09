/*
    pt.Live - Live Singing DAW
    TrackManager implementation
*/

#include "include/Audio/TrackManager.h"
#include "include/Audio/PluginWindow.h"
#include "include/Audio/PtLog.h"

#include <set>
#include <vector>

// ChannelMapperPlugin only exists in recent Tracktion Engine versions. Without it the mono fold is
// skipped (and logged) instead of breaking the build.
#if defined (__has_include)
 #if __has_include(<tracktion_engine/plugins/internal/tracktion_ChannelMapperPlugin.h>)
  #define PTL_HAS_CHANNEL_MAPPER 1
 #endif
#endif
#ifndef PTL_HAS_CHANNEL_MAPPER
 #define PTL_HAS_CHANNEL_MAPPER 0
#endif

using namespace te;

namespace
{
    const juce::Identifier kindId ("ptlKind");

    // n is 1-based. The FIRST stereo / mono input are the defaults "BEAT" / "MICRO";
    // every later one is numbered: "Stereo In 2", "Mono In 2". FX channels: "FX 1", "FX 2", ...
    juce::String nameFor (TrackManager::TrackType t, int n)
    {
        switch (t)
        {
            case TrackManager::TrackType::StereoIn:  return n == 1 ? juce::String ("BEAT")  : "Stereo In " + juce::String (n);
            case TrackManager::TrackType::MonoIn:    return n == 1 ? juce::String ("MICRO") : "Mono In "   + juce::String (n);
            case TrackManager::TrackType::FXChannel: return "FX " + juce::String (n);
            case TrackManager::TrackType::Master:    return "MASTER";
        }
        return "Track " + juce::String (n);
    }
}

TrackManager::TrackManager(te::Engine& engine, te::Edit& edit)
    : engineRef(engine), editRef(edit)
{
    edit.ensureMasterTrack();

    // Pre-fader sends no longer exist. Sessions saved by older builds may still hold sends
    // flagged "ptlSendPre": clear the flag and move them to the end of the chain (post-fader).
    {
        const juce::Identifier legacyPreId ("ptlSendPre");

        for (auto* t : te::getAudioTracks (editRef))
        {
            juce::Array<te::Plugin::Ptr> legacy;

            for (auto* p : t->pluginList.getPlugins())
                if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
                    if (send->state.hasProperty (legacyPreId))
                        legacy.add (te::Plugin::Ptr (send));

            for (auto& p : legacy)
            {
                p->state.removeProperty (legacyPreId, nullptr);
                t->pluginList.insertPlugin (p, -1, nullptr);
            }
        }
    }

    // A loaded session that already holds a Send Master keeps working even if its Master
    // Return is gone. Old sessions without any Send Master are left untouched.
    for (auto* t : te::getAudioTracks (editRef))
        if (hasMasterSend (t))
        {
            ensureMasterSendReturn();
            break;
        }
}

juce::String TrackManager::makeUniqueName (TrackType type) const
{
    for (int n = 1; n < 1000; ++n)
    {
        const auto candidate = nameFor (type, n);
        bool used = false;

        for (auto* t : te::getAudioTracks (editRef))
            if (t != nullptr && t->getName() == candidate)
                used = true;

        if (! used)
            return candidate;
    }
    return nameFor (type, 1000);
}

void TrackManager::ensureDefaultTracks()
{
    if (! te::getAudioTracks (editRef).isEmpty())
        return;

    createTrack (TrackType::StereoIn);     // BEAT
    createTrack (TrackType::MonoIn);       // MICRO
    createTrack (TrackType::FXChannel);    // FX 1
    createTrack (TrackType::FXChannel);    // FX 2
}

int TrackManager::findFreeFxBus() const
{
    for (int bus = 0; bus < 64; ++bus)
        if (getFxTrackForBus (bus) == nullptr)
            return bus;
    return 0;
}

te::AudioTrack* TrackManager::createTrack(TrackType type, int insertIndex)
{
    if (type == TrackType::Master)
        return nullptr;   // master is auto-created by the Edit

    // Append after the last top-level audio track.
    Track* preceding = nullptr;
    auto existing = te::getAudioTracks(editRef);

    if (insertIndex >= 0 && insertIndex < existing.size())
        preceding = existing[insertIndex];
    else if (! existing.isEmpty())
        preceding = existing.getLast();

    TrackInsertPoint insertPoint { nullptr, preceding };

    const auto name = makeUniqueName (type);

    AudioTrack::Ptr newTrack = editRef.insertNewAudioTrack (insertPoint, nullptr, true);

    if (newTrack == nullptr)
        return nullptr;

    switch (type)
    {
        case TrackType::MonoIn:
            newTrack->setMaxNumOfInputs (1);
            newTrack->getWaveInputDevice().setChannelConfiguration (ChannelConfiguration::mono());
            break;

        case TrackType::StereoIn:
            newTrack->setMaxNumOfInputs (2);
            newTrack->getWaveInputDevice().setChannelConfiguration (ChannelConfiguration::stereo());
            break;

        case TrackType::FXChannel:
        {
            newTrack->getWaveInputDevice().setEnabled (false);

            // An FX channel receives its audio from the AuxSend plugins of other tracks
            // through an AuxReturn plugin sharing the same bus number.
            const int bus = findFreeFxBus();
            if (auto plugin = editRef.getPluginCache().createNewPlugin (AuxReturnPlugin::xmlTypeName, {}))
            {
                if (auto* ret = dynamic_cast<AuxReturnPlugin*> (plugin.get()))
                    ret->busNumber = bus;

                newTrack->pluginList.insertPlugin (plugin, 0, nullptr);   // before the fader
            }
            break;
        }

        case TrackType::Master:
            break;
    }

    // Every new track starts at unity gain, whatever the engine default is.
    if (auto* vp = newTrack->getVolumePlugin())
        vp->setVolumeDb (0.0f);

    newTrack->state.setProperty (kindId, static_cast<int> (type), nullptr);
    newTrack->setName (name);

    return newTrack.get();
}

bool TrackManager::deleteTrack(te::AudioTrack* track)
{
    if (track == nullptr)
        return false;

    // FX channel: first remove every send that points at its bus, on EVERY other audio
    // track (input tracks and other FX channels alike).
    if (getTrackType (track) == TrackType::FXChannel)
    {
        const int bus = getFxBus (track);

        if (bus >= 0)
            for (auto* t : te::getAudioTracks (editRef))
                if (t != nullptr && t != track)
                    for (auto* send : getSends (t))
                        if (send != nullptr && send->busNumber.get() == bus)
                            send->deleteFromParent();
    }

    editRef.deleteTrack (track);
    return true;
}

void TrackManager::renameTrack(te::Track* track, const juce::String& newName)
{
    if (track != nullptr)
        track->setName (newName);
}

juce::Array<te::AudioTrack*> TrackManager::getAllTracks() const
{
    return te::getAudioTracks (editRef);
}

juce::Array<te::AudioTrack*> TrackManager::getTracksOfType (TrackType type) const
{
    juce::Array<te::AudioTrack*> result;

    for (auto* t : te::getAudioTracks (editRef))
        if (t != nullptr && getTrackType (t) == type)
            result.add (t);

    return result;
}

te::MasterTrack* TrackManager::getMasterTrack() const
{
    return editRef.getMasterTrack();
}

te::AudioTrack* TrackManager::getTrackAt (int index) const
{
    auto allTracks = te::getAudioTracks (editRef);

    if (index >= 0 && index < allTracks.size())
        return allTracks[index];

    return nullptr;
}

int TrackManager::getNumTracks() const
{
    return te::getAudioTracks (editRef).size();
}

juce::String TrackManager::getTypeName (TrackType type)
{
    switch (type)
    {
        case TrackType::MonoIn:    return "Mono Input";
        case TrackType::StereoIn:  return "Stereo Input";
        case TrackType::FXChannel: return "FX Channel";
        case TrackType::Master:    return "Master";
    }
    return "Unknown";
}

TrackManager::TrackType TrackManager::getTrackType (te::Track* track) const
{
    if (track == nullptr)
        return TrackType::MonoIn;

    if (static_cast<te::Track*> (editRef.getMasterTrack()) == track)
        return TrackType::Master;

    if (track->state.hasProperty (kindId))
        return static_cast<TrackType> (static_cast<int> (track->state.getProperty (kindId)));

    if (auto* at = dynamic_cast<te::AudioTrack*> (track))
        return at->getMaxNumOfInputs() >= 2 ? TrackType::StereoIn : TrackType::MonoIn;

    return TrackType::MonoIn;
}

te::VolumeAndPanPlugin* TrackManager::getVolumePlugin (te::Track* track) const
{
    if (track == nullptr)
        return nullptr;

    if (auto* at = dynamic_cast<te::AudioTrack*> (track))
        return at->getVolumePlugin();

    if (getTrackType (track) == TrackType::Master)
    {
        te::VolumeAndPanPlugin* vp = editRef.getMasterVolumePlugin();   // works for raw ptr or Ptr
        return vp;
    }

    return nullptr;
}

te::PluginList& TrackManager::getPluginList (te::Track* track) const
{
    if (auto* at = dynamic_cast<te::AudioTrack*> (track))
        return at->pluginList;

    return editRef.getMasterPluginList();
}

juce::String TrackManager::describeTrack (te::Track* track) const
{
    if (track == nullptr)
        return "track <null>";

    juce::String s;
    s << "track '" << track->getName() << "' kind=" << getTypeName (getTrackType (track));

    if (auto* at = dynamic_cast<te::AudioTrack*> (track))
        s << " maxIn=" << at->getMaxNumOfInputs();

    s << " | chain:";

    int index = 0;
    for (auto* p : getPluginList (track).getPlugins())
        if (p != nullptr)
            s << "\n        [" << index++ << "] " << ptl::diag::describePlugin (*p);

    return s;
}

void TrackManager::logTrackState (const juce::String& tag, te::Track* track) const
{
    ptl::diag::write ("[" + tag + "] " + describeTrack (track));
}

// ---------------------------------------------------------------------------------------------
// Mono-layout probing (MONO IN tracks)
// ---------------------------------------------------------------------------------------------
namespace
{
    using Layout = juce::AudioProcessor::BusesLayout;

    // The layout we WANT for a plugin on a MONO IN track:
    //   main input bus  = mono
    //   main output bus = mono
    //   every other (aux / sidechain) bus = disabled, so the engine never has to feed it.
    // Returns false for plugins that have no main audio input or output (instruments, MIDI FX).
    bool buildMonoLayout (juce::AudioProcessor& proc, Layout& target)
    {
        target = proc.getBusesLayout();

        if (target.inputBuses.isEmpty() || target.outputBuses.isEmpty())
            return false;

        target.inputBuses.getReference (0)  = juce::AudioChannelSet::mono();
        target.outputBuses.getReference (0) = juce::AudioChannelSet::mono();

        for (int i = 1; i < target.inputBuses.size(); ++i)
            target.inputBuses.getReference (i) = juce::AudioChannelSet::disabled();

        for (int i = 1; i < target.outputBuses.size(); ++i)
            target.outputBuses.getReference (i) = juce::AudioChannelSet::disabled();

        return true;
    }

    // True when the plugin already runs mono-in / mono-out with no aux bus enabled.
    bool isAlreadyMono (const Layout& current, const Layout& wanted)
    {
        return current == wanted;
    }

    enum class MonoProbe { alreadyMono, canSwitch, unsupported, notApplicable };

    const char* toText (MonoProbe r)
    {
        switch (r)
        {
            case MonoProbe::alreadyMono:   return "alreadyMono";
            case MonoProbe::canSwitch:     return "accepts-mono";
            case MonoProbe::unsupported:   return "refuses-mono";
            case MonoProbe::notApplicable: return "n/a";
        }
        return "?";
    }

    // Plugins (by edit-item id) that refused / failed a mono layout once WITH THE GRAPH RELEASED.
    // They are not probed again, so inserting another plugin does not glitch the audio each time.
    // Message thread only.
    std::set<juce::uint64>& knownStereoOnly()
    {
        static std::set<juce::uint64> ids;
        return ids;
    }

    // Pure query: changes nothing, safe while the graph is running.
    MonoProbe probeMono (juce::AudioProcessor& proc, Layout& wanted, juce::String& why)
    {
        if (! buildMonoLayout (proc, wanted))
        {
            why = "no main audio in/out bus";
            return MonoProbe::notApplicable;
        }

        if (isAlreadyMono (proc.getBusesLayout(), wanted))
            return MonoProbe::alreadyMono;

        if (proc.checkBusesLayoutSupported (wanted))
            return MonoProbe::canSwitch;

        // Diagnostics only: would the plugin at least take mono in -> stereo out?
        auto monoToStereo = wanted;
        monoToStereo.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        why = proc.checkBusesLayoutSupported (monoToStereo)
                ? "mono in/out refused, but mono-in -> stereo-out is accepted"
                : "mono in/out refused (plugin is stereo-only)";
        return MonoProbe::unsupported;
    }
}


bool TrackManager::applyTrackChannelLayoutNoRestart (te::AudioTrack* track) const
{
    if (track == nullptr)
        return false;

    if (getTrackType (track) != TrackType::MonoIn)
    {
        ptl::diag::write ("[mono-fix] skipped: '" + track->getName() + "' is not a MONO IN track");
        return false;
    }

    // Forcing a mono main bus (setBusesLayout) made Pro-Q 3 and Auto-Tune Pro go "dead" on the
    // MICRO track (windows idle, no analyser / pitch movement) while the plugins that were
    // ALREADY mono (CLA-3A Mono, RVox Mono) kept working. The two hidden channel mappers
    // (ensureMonoFold) already keep the track truly mono, so the plugins are left in the
    // layout the engine loaded them with. Set kForceMonoBuses = true to get the old behaviour.
    constexpr bool kForceMonoBuses = false;

    if (! kForceMonoBuses)
    {
        for (auto* plugin : track->pluginList.getPlugins())
            if (auto* ext = dynamic_cast<te::ExternalPlugin*> (plugin))
                ptl::diag::write ("[mono-fix] layout kept as loaded: " + ptl::diag::describePlugin (*ext));
        return false;
    }

    // MESSAGE THREAD ONLY.  Returns true when the playback graph was RELEASED, meaning the caller
    // must restartPlayback() to rebuild it (also when no layout ended up changing).
    //
    // Phase 1 - SELECT (no probing of the plugin yet, graph keeps running):
    //   keep only loaded plugins that are not already mono and not remembered as stereo-only.
    //   If nobody is left the audio is never touched.
    // Phase 2 - PROBE + APPLY with the graph RELEASED:
    //   a hosted VST3 can answer checkBusesLayoutSupported() differently while it is active
    //   (the log showed Pro-Q 3 flipping between "refused" and "accepted but setBusesLayout fails"),
    //   so the answer that counts is the one given while no audio thread is inside the plugin.
    //   A live probe is still logged first, only to confirm / refute that theory.

    struct Candidate { te::ExternalPlugin* ext; juce::AudioPluginInstance* inst; MonoProbe live; };
    std::vector<Candidate> todo;

    for (auto* plugin : track->pluginList.getPlugins())
    {
        auto* ext = dynamic_cast<te::ExternalPlugin*> (plugin);
        if (ext == nullptr)
            continue;

        const auto name = "'" + ext->getName() + "' on '" + track->getName() + "'";

        if (ptl::pluginhost::isMissing (*ext))
        {
            ptl::diag::write ("[mono-fix] " + name + ": not in the scanned plugin list, skipped");
            continue;                                   // never try to load it (could freeze the app)
        }

        if (ext->getAudioPluginInstance() == nullptr)
        {
            ptl::diag::write ("[mono-fix] " + name + ": not loaded yet, initialiseFully()");
            ext->initialiseFully();                     // restored sessions instantiate lazily
        }

        auto* instance = ext->getAudioPluginInstance();

        if (instance == nullptr)
        {
            ptl::diag::write ("[mono-fix] " + name + ": still initializing, will be ready shortly");
            continue;
        }

        Layout wanted;

        if (! buildMonoLayout (*instance, wanted))
        {
            ptl::diag::write ("[mono-fix] " + name + ": skipped - no main audio in/out bus");
            continue;
        }

        if (isAlreadyMono (instance->getBusesLayout(), wanted))
        {
            ptl::diag::write ("[mono-fix] " + name + ": already mono, nothing to do");
            continue;
        }

        if (knownStereoOnly().count (ext->itemID.getRawID()) > 0)
        {
            ptl::diag::write ("[mono-fix] " + name + ": known stereo-only, not probed again "
                              "(needs a dual-mono fold around it)");
            continue;
        }

        juce::String ignored;
        todo.push_back ({ ext, instance, probeMono (*instance, wanted, ignored) });
    }

    if (todo.empty())
        return false;

    // Phase 2. Freeing an already-freed context is harmless.
    const bool released = ! editRef.isLoading();

    if (released)
        editRef.getTransport().freePlaybackContext();

    for (auto& c : todo)
    {
        const auto name = "'" + c.ext->getName() + "' on '" + track->getName() + "'";

        c.inst->releaseResources();                     // no-op if the graph already released it

        Layout wanted;
        juce::String why;
        const auto settled = probeMono (*c.inst, wanted, why);

        ptl::diag::write ("[mono-fix] " + name + ": probe live=" + toText (c.live)
                          + " released=" + toText (settled)
                          + (c.live != settled ? "  <-- answer depends on graph state" : ""));

        if (settled == MonoProbe::canSwitch)
        {
            if (c.inst->setBusesLayout (wanted))
            {
                ptl::diag::write ("[mono-fix] " + name + ": setBusesLayout(mono) OK -> "
                                  + ptl::diag::describePlugin (*c.ext));
                continue;
            }

            ptl::diag::write ("[mono-fix] " + name + ": setBusesLayout(mono) FAILED although "
                              "checkBusesLayoutSupported said yes -> stereo-only from now on");
        }
        else
        {
            ptl::diag::write ("[mono-fix] " + name + ": NOT switchable - " + why
                              + " (needs a dual-mono fold around it)");
        }

        knownStereoOnly().insert (c.ext->itemID.getRawID());
    }

    return released;
}

void TrackManager::ensureMonoFold (te::AudioTrack* track) const
{
    // MONO IN track + stereo-only plugins (Pro-Q 3, Auto-Tune Pro) need a "dual mono" wrapper:
    //   [fold IN ]  copies channel 0 into channel 1 FIRST, so a stereo plugin sees L == R
    //               (instead of L = voice, R = silence) and processes the voice normally.
    //   [plugins ]  mono plugins (CLA-3A, RVox) process channel 0 only; stereo plugins process both.
    //   [fold OUT]  copies channel 0 into channel 1 LAST, so whatever a stereo plugin did to the
    //               right channel is discarded and the track leaves truly mono (L == R).
    // Both are hidden Channel Mappers (map 0:0, 0:1); the inspector only lists ExternalPlugins.
    if (track == nullptr || getTrackType (track) != TrackType::MonoIn)
        return;

#if PTL_HAS_CHANNEL_MAPPER
    static const juce::Identifier foldOutId ("ptlMonoFold");     // last, right before Volume & Pan
    static const juce::Identifier foldInId  ("ptlMonoFoldIn");   // first in the chain

    auto& list = track->pluginList;

    auto makeFold = [this] (const juce::Identifier& id) -> te::Plugin::Ptr
    {
        auto plugin = editRef.getPluginCache().createNewPlugin (te::ChannelMapperPlugin::xmlTypeName,
                                                                juce::PluginDescription());
        if (plugin == nullptr)
            return nullptr;

        if (auto* cm = dynamic_cast<te::ChannelMapperPlugin*> (plugin.get()))
        {
            cm->setNumInputChannels (2);
            cm->setNumOutputChannels (2);
        }

        plugin->state.setProperty ("channelMap", "0:0,0:1", nullptr);   // L -> L and L -> R
        plugin->state.setProperty (id, true, nullptr);
        plugin->setEnabled (true);
        return plugin;
    };

    struct Pos { te::Plugin* in = nullptr; te::Plugin* out = nullptr; int inIdx = -1, outIdx = -1, volIdx = -1; };

    auto scan = [&list]
    {
        Pos r; int i = 0;
        for (auto* p : list.getPlugins())
        {
            if (p != nullptr)
            {
                if (r.in  == nullptr && p->state.getProperty (foldInId,  false)) { r.in  = p; r.inIdx  = i; }
                if (r.out == nullptr && p->state.getProperty (foldOutId, false)) { r.out = p; r.outIdx = i; }
                if (r.volIdx < 0 && dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr) r.volIdx = i;
            }
            ++i;
        }
        return r;
    };

    auto pos = scan();

    if (pos.volIdx < 0)
        return;                                         // no fader yet: nothing to anchor to

    // ---- fold OUT: right before Volume & Pan ------------------------------------------------
    if (! (pos.out != nullptr && pos.outIdx == pos.volIdx - 1))
    {
        if (pos.out != nullptr)
        {
            pos.out->deleteFromParent();
            pos = scan();
        }

        if (! list.canInsertPlugin())
        {
            ptl::diag::write ("[mono-fix] cannot add the mono fold on '" + track->getName() + "': plugin limit reached");
            return;
        }

        if (auto plugin = makeFold (foldOutId))
        {
            list.insertPlugin (plugin, pos.volIdx, nullptr);
            ptl::diag::write ("[mono-fix] mono fold OUT (L -> R) placed before Volume & Pan on '" + track->getName() + "'");
        }
        else
        {
            ptl::diag::write ("[mono-fix] could not create the mono fold plugin on '" + track->getName() + "'");
            return;
        }

        pos = scan();
    }

    // ---- fold IN: first plugin of the chain -------------------------------------------------
    if (! (pos.in != nullptr && pos.inIdx == 0))
    {
        if (pos.in != nullptr)
        {
            pos.in->deleteFromParent();
            pos = scan();
        }

        if (! list.canInsertPlugin())
        {
            ptl::diag::write ("[mono-fix] cannot add the mono fold IN on '" + track->getName() + "': plugin limit reached");
            return;
        }

        if (auto plugin = makeFold (foldInId))
        {
            list.insertPlugin (plugin, 0, nullptr);
            ptl::diag::write ("[mono-fix] mono fold IN (L -> R) placed first on '" + track->getName() + "'");
        }
    }
#else
    static bool warned = false;
    if (! warned)
    {
        warned = true;
        ptl::diag::write ("[mono-fix] this Tracktion Engine has no ChannelMapperPlugin: L/R mono fold unavailable");
    }
#endif
}

void TrackManager::rebuildGraphAfterRelease (te::Track* track) const
{
    // freePlaybackContext() destroyed the context. Edit::restartPlayback() only restarts an
    // EXISTING context, so on its own it leaves the graph un-built: no audio reaches the output
    // until something else (the user re-picking the input) calls ensureContextAllocated().
    const bool hadContext = editRef.getCurrentPlaybackContext() != nullptr;

    editRef.getTransport().ensureContextAllocated();

    if (hadContext)                                 // the free did not really happen: rebuild anyway
        editRef.restartPlayback();

    ptl::diag::write ("[mono-fix] playback graph re-created" + juce::String (hadContext ? " (+restart)" : "")
                      + " -> input routing will be re-applied");

    if (onGraphRebuilt)
        onGraphRebuilt (track);
}

void TrackManager::applyTrackChannelLayout (te::AudioTrack* track) const
{
    ensureMonoFold (track);

    if (applyTrackChannelLayoutNoRestart (track) && ! editRef.isLoading())
        rebuildGraphAfterRelease (track);
}

void TrackManager::applyAllTrackChannelLayouts() const
{
    bool anyChanged = false;

    for (auto* t : te::getAudioTracks (editRef))
    {
        ensureMonoFold (t);
        anyChanged = applyTrackChannelLayoutNoRestart (t) || anyChanged;
    }

    if (anyChanged && ! editRef.isLoading())
        rebuildGraphAfterRelease (nullptr);         // once, not once per track; nullptr = every track
}

int TrackManager::getFxBus (te::AudioTrack* fxTrack) const
{
    if (fxTrack == nullptr)
        return -1;

    if (auto* ret = fxTrack->pluginList.findFirstPluginOfType<te::AuxReturnPlugin>())
        return ret->busNumber.get();

    return -1;
}

te::AudioTrack* TrackManager::getFxTrackForBus (int bus) const
{
    if (bus == kMasterSendBus)
        return nullptr;                 // reserved for the Send Master, never an FX channel

    for (auto* t : te::getAudioTracks (editRef))
        if (t != nullptr && getTrackType (t) == TrackType::FXChannel && getFxBus (t) == bus)
            return t;

    return nullptr;
}

juce::Array<te::AuxSendPlugin*> TrackManager::getSends (te::AudioTrack* track) const
{
    juce::Array<te::AuxSendPlugin*> sends;

    if (track == nullptr)
        return sends;

    for (auto* p : track->pluginList.getPlugins())
        if (auto* s = dynamic_cast<te::AuxSendPlugin*> (p))
            sends.add (s);

    return sends;
}

bool TrackManager::isMasterSend (te::AuxSendPlugin* send) const
{
    return send != nullptr && send->busNumber.get() == kMasterSendBus;
}

juce::String TrackManager::getSendTargetName (te::AuxSendPlugin* send) const
{
    if (send == nullptr)
        return "(missing FX)";

    if (isMasterSend (send))
        return "MASTER";

    if (auto* fx = getFxTrackForBus (send->busNumber.get()))
        return fx->getName();

    return "(missing FX)";
}

bool TrackManager::hasMasterSend (te::AudioTrack* track) const
{
    for (auto* s : getSends (track))
        if (isMasterSend (s))
            return true;

    return false;
}

bool TrackManager::canHaveMasterSend (te::AudioTrack* track) const
{
    // Mono In, Stereo In AND FX channels may have a Send Master (at most one per track).
    // A Master send never creates a feedback loop: the Master Return sits at the head of the
    // master chain and nothing downstream of it feeds back into a track.
    const auto type = getTrackType (track);
    return track != nullptr && (type == TrackType::MonoIn || type == TrackType::StereoIn
                                || type == TrackType::FXChannel);
}

// Depth-first walk over the existing sends: can we get from "from" to "goal" by following
// AuxSend.busNumber -> getFxTrackForBus? "visited" also keeps a (hand-edited) looping session
// from recursing forever. Disabled sends count too: their plugin is still part of the graph.
bool TrackManager::canReachFx (te::AudioTrack* from, te::AudioTrack* goal,
                               juce::Array<te::AudioTrack*>& visited) const
{
    if (from == nullptr)
        return false;

    if (from == goal)
        return true;

    if (visited.contains (from))
        return false;

    visited.add (from);

    for (auto* s : getSends (from))
        if (s != nullptr && ! isMasterSend (s))
            if (canReachFx (getFxTrackForBus (s->busNumber.get()), goal, visited))
                return true;

    return false;
}

bool TrackManager::wouldCreateSendLoop (te::AudioTrack* source, te::AudioTrack* targetFx) const
{
    if (source == nullptr || targetFx == nullptr)
        return false;

    // source -> target closes a loop when target already (indirectly) feeds source.
    // target == source is the shortest loop (canReachFx returns true immediately).
    juce::Array<te::AudioTrack*> visited;
    const bool loop = canReachFx (targetFx, source, visited);

    if (loop)
        ptl::diag::write ("[send] refused '" + source->getName() + "' -> '" + targetFx->getName()
                          + "': it would create a feedback loop");

    return loop;
}

juce::Array<te::AudioTrack*> TrackManager::getSendCandidates (te::AudioTrack* source) const
{
    juce::Array<te::AudioTrack*> result;

    if (source == nullptr)
        return result;

    juce::Array<int> usedBuses;
    for (auto* s : getSends (source))
        usedBuses.add (s->busNumber.get());

    for (auto* fx : getTracksOfType (TrackType::FXChannel))
    {
        if (fx == source || usedBuses.contains (getFxBus (fx)))
            continue;

        // Input tracks can never be fed by an FX channel, so only an FX source can loop.
        if (getTrackType (source) == TrackType::FXChannel && wouldCreateSendLoop (source, fx))
            continue;

        result.add (fx);
    }

    return result;
}

te::AuxReturnPlugin* TrackManager::ensureMasterSendReturn()
{
    auto& masterPlugins = editRef.getMasterPluginList();

    for (auto* p : masterPlugins.getPlugins())
        if (auto* ret = dynamic_cast<te::AuxReturnPlugin*> (p))
            if (ret->busNumber.get() == kMasterSendBus)
                return ret;                                   // already there (idempotent)

    if (auto plugin = editRef.getPluginCache().createNewPlugin (AuxReturnPlugin::xmlTypeName, {}))
    {
        auto* ret = dynamic_cast<AuxReturnPlugin*> (plugin.get());

        if (ret == nullptr)
            return nullptr;

        ret->busNumber = kMasterSendBus;

        // HEAD of the master chain: the send signal is summed in BEFORE the master inserts,
        // the master fader and the level meter, so it is processed (and metered / recorded by
        // the MasterRecorder tap) like any track signal. It is an AuxReturnPlugin, not an
        // ExternalPlugin, so the INSERTS list of the master never shows it.
        masterPlugins.insertPlugin (plugin, 0, nullptr);

        ptl::diag::write ("[send] Master Return created (bus " + juce::String (kMasterSendBus)
                          + ") at the head of the master chain");
        return ret;
    }

    ptl::diag::write ("[send] could NOT create the Master Return");
    return nullptr;
}

//==============================================================================
// Sends (post-fader only)
te::AuxSendPlugin* TrackManager::addSend (te::AudioTrack* source, int bus)
{
    if (source == nullptr)
        return nullptr;

    if (auto plugin = editRef.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {}))
    {
        // Always post-fader: insert AFTER the whole chain (-1), i.e. after the volume fader.
        source->pluginList.insertPlugin (plugin, -1, nullptr);

        if (auto* send = dynamic_cast<te::AuxSendPlugin*> (plugin.get()))
        {
            send->busNumber = bus;
            send->setGainDb (-16.0f);                       // Cubase default; user can change it

            ptl::diag::write ("[send] " + source->getName() + " -> bus " + juce::String (bus)
                              + " | POST-fader | gain=" + juce::String (send->getGainDb(), 1) + " dB");
            return send;
        }
    }

    ptl::diag::write ("[send] could NOT create the send to bus " + juce::String (bus));
    return nullptr;
}
