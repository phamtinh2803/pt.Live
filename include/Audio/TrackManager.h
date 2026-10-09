/*
    pt.Live - Live Singing DAW
    TrackManager - manages creation/deletion of tracks

    Handles:
    - Mono In, Stereo In, FX Channel (+ the always-present Master)
    - The track kind is stored IN the track's ValueTree ("ptlKind"), so it can never
      get out of sync when tracks are added / removed / reordered.
    - FX channels own an AuxReturn plugin with a unique bus number; input tracks AND other
      FX channels reach them through AuxSend plugins (the "Sends" list in the inspector).
      FX -> FX sends that would create a feedback loop are refused (wouldCreateSendLoop).
    - "Send Master" (Mono In / Stereo In / FX channels): a normal AuxSend whose bus is kMasterSendBus.
      The matching AuxReturn lives at the HEAD of the master plugin chain (created lazily by
      ensureMasterSendReturn), so the signal goes through the master inserts / fader / meter
      and the MasterRecorder tap like any other signal.
*/

#pragma once

#include <JuceHeader.h>
#include <functional>
#include "include/Audio/TracktionHeaders.h"

class TrackManager {
public:
    enum class TrackType { MonoIn, StereoIn, FXChannel, Master };

    // Reserved bus number of the Send Master. FX buses are 0..63 (findFreeFxBus), so this
    // never collides with an FX channel; getFxTrackForBus() never returns a track for it.
    static constexpr int kMasterSendBus = 1000;

    TrackManager(te::Engine& engine, te::Edit& edit);

    // Called (message thread) after the playback graph was released and re-created to change a
    // plugin bus layout. A new graph starts with NO input routing, so whoever owns the input
    // choice (the UI rows) must route the physical input to its track again.
    std::function<void (te::Track*)> onGraphRebuilt;
    ~TrackManager() = default;

    // Track lifecycle
    te::AudioTrack* createTrack(TrackType type, int insertIndex = -1);
    bool deleteTrack(te::AudioTrack* track);
    void renameTrack(te::Track* track, const juce::String& newName);

    // Default session layout: BEAT (stereo in), MICRO (mono in), FX 1, FX 2.
    // Only does something when the Edit has no audio track at all (fresh session).
    // Later tracks are named "Stereo In 2", "Mono In 2", "FX 3", ...
    void ensureDefaultTracks();

    // Accessors
    te::Edit& getEdit() const noexcept { return editRef; }
    juce::Array<te::AudioTrack*> getAllTracks() const;
    juce::Array<te::AudioTrack*> getTracksOfType(TrackType type) const;
    te::MasterTrack* getMasterTrack() const;
    te::AudioTrack* getTrackAt(int index) const;
    int getNumTracks() const;

    // Type helpers
    static juce::String getTypeName(TrackType type);
    TrackType getTrackType(te::Track* track) const;

    // Plugin helpers (work for audio tracks AND the master track)
    te::VolumeAndPanPlugin* getVolumePlugin(te::Track* track) const;
    te::PluginList& getPluginList(te::Track* track) const;

    // Keeps a MONO IN track mono: every external plugin on it is switched to a mono main bus
    // (in AND out) when it supports that. Without this a VST3 keeps its default 2-in/2-out
    // layout and silently turns the whole mono chain into a stereo one. Call it after inserting
    // a plugin and after loading a session. Plugins that only support stereo are left alone.
    // When a layout was really changed the playback graph is rebuilt (restartPlayback): the graph
    // is built from the plugin's bus layout at that moment, so without the rebuild it keeps the
    // mono -> stereo up-mix it created for the plugin's default 2-in/2-out layout and the track
    // stays stereo even though the plugin itself is already mono.
    void applyTrackChannelLayout(te::AudioTrack* track) const;
    void applyAllTrackChannelLayouts() const;

    // Diagnostics: "track 'MONO IN 1' kind=MonoIn maxIn=1 | chain: [0] ..., [1] ..." and a logger.
    juce::String describeTrack(te::Track* track) const;
    void logTrackState(const juce::String& tag, te::Track* track) const;

    // FX bus / sends
    int getFxBus(te::AudioTrack* fxTrack) const;                 // -1 if none
    te::AudioTrack* getFxTrackForBus(int bus) const;
    juce::Array<te::AuxSendPlugin*> getSends(te::AudioTrack* track) const;

    // Send targets: a send goes either to an FX channel (bus 0..63) or to the Send Master.
    bool isMasterSend(te::AuxSendPlugin* send) const;
    juce::String getSendTargetName(te::AuxSendPlugin* send) const;   // "Master" / FX name / "(missing FX)"
    bool hasMasterSend(te::AudioTrack* track) const;
    bool canHaveMasterSend(te::AudioTrack* track) const;             // Mono In / Stereo In / FX channel

    // True when adding a send source -> targetFx would close a loop (or target == source),
    // following the existing FX -> FX sends.
    bool wouldCreateSendLoop(te::AudioTrack* source, te::AudioTrack* targetFx) const;

    // FX channels the "Add send" menu of this track may offer: not sent to yet, not the track
    // itself and (so FX -> FX works) not creating a feedback loop.
    juce::Array<te::AudioTrack*> getSendCandidates(te::AudioTrack* source) const;

    // Makes sure the Master Return exists (head of the master plugin chain). Idempotent.
    // Call it before adding a Send Master; the constructor also calls it when a loaded
    // session already holds a Send Master.
    te::AuxReturnPlugin* ensureMasterSendReturn();

    // Add a POST-fader send from `source` to a bus (FX bus, or kMasterSendBus for the Master),
    // then return the new plugin (or nullptr). Pre-fader sends are not supported.
    te::AuxSendPlugin* addSend(te::AudioTrack* source, int bus);

private:
    void ensureMonoFold(te::AudioTrack* track) const;                      // hidden L->R fold, last insert on a MONO IN track
    void rebuildGraphAfterRelease(te::Track* track) const;                 // re-create context + re-route inputs
    bool applyTrackChannelLayoutNoRestart(te::AudioTrack* track) const;   // true = a layout was changed
    int findFreeFxBus() const;
    bool canReachFx(te::AudioTrack* from, te::AudioTrack* goal, juce::Array<te::AudioTrack*>& visited) const;
    juce::String makeUniqueName(TrackType type) const;

    te::Engine& engineRef;
    te::Edit& editRef;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackManager)
};
