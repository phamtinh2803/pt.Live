/*
    pt.Live - Live Singing DAW
    LiveController - the "what the singer does on stage" logic, no UI.

    Replaces the old ptStudio remote commands. It owns:
      - BEAT / MICRO on-off (track mute), VANG on-off (all FX sends of MICRO, not the Master Send)
      - TONE up/down  : real pitch shift (every SoundShifter Pitch, semitones, on ANY track)
      - KEY  -/+      : only moves the key given to Autotune (on ANY track; the music pitch is untouched)
      - the key found by the VPS : sets Autotune key/scale
      - user-made SLIDERs (any VST parameter / send level) and TOGGLEs (any on-off the app has), see CtlItem

    Final Autotune key = detected key shifted by (toneSteps + keyOffset) semitones.
    A new SONG (resetShifts, called the moment it is detected) puts the music back to its original pitch
    and clears both offsets, same as ptStudio. The key that arrives later from the VPS does not touch
    them, so the singer may already press TONE / KEY while the server is still working.

    Every method must be called on the message thread.
*/

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <functional>
#include "include/Audio/TrackManager.h"

class LiveController
{
public:
    explicit LiveController (TrackManager& tm);

    //== Toggles (read the REAL track state, so the mixer and this panel always agree) ==
    bool isBeatOn() const;
    bool isMicOn()  const;
    bool isVangOn() const;                 // true when at least one FX send of MICRO is enabled
    bool hasVang()  const;                 // MICRO has at least one FX send

    void setBeatOn (bool on);
    void setMicOn  (bool on);
    void setVangOn (bool on);

    //== Levels: BEAT / MICRO fader (dB, same fader as the mixer) =========================
    enum class Channel { Beat, Mic };
    bool  hasVolume   (Channel) const;
    float getVolumeDb (Channel) const;                     // 0 dB when the track is missing
    void  setVolumeDb (Channel, float db);

    //== Auto-Tune knobs on MICRO (normalised 0..1; the text is what the plugin itself shows) ==
    enum class AutotuneKnob { RetuneSpeed, FlexTune };
    float        getAutotuneKnob (AutotuneKnob) const;     // -1 when Autotune / the parameter is not found
    void         setAutotuneKnob (AutotuneKnob, float norm01);
    juce::String getAutotuneKnobText (AutotuneKnob, float norm01) const;

    //== User-made controls: any number of SLIDERs and TOGGLEs (the panel's "+" button) ===============
    // They are stored in the session itself (property "ptlControlItems" of the Edit, one JSON text), so a .pt
    // keeps them. A target is remembered by id (track id, plugin id, parameter id), so renaming a track or a
    // plugin never breaks a control. Sessions made before this existed (TOGGLE 1..4 and the FX send slider,
    // both stored on MICRO) are converted the first time they are read.
    //
    //   ParamSlider : ONE parameter of ONE VST on ONE track. The parameter list is what the VST publishes.
    //   SendSlider  : send level of ONE track to ONE FX (or Master) bus.
    //   VolumeSlider: fader (dB) of ONE track (also Master).   PanSlider: pan of ONE track (also Master).
    //   Toggle      : exactly ONE function (Fn). For every Fn "lit" means "on / sound passes".
    struct CtlItem
    {
        // EmptySlider / Fn::None = placeholders made for the user (nothing assigned yet, "unassigned").
        // Fn::AllEffects = every INSERT and every SEND of EVERY track (Master too): off = dry sound (in = out).
        enum class Kind { ParamSlider, SendSlider, VolumeSlider, PanSlider, Toggle, EmptySlider };
        enum class Fn   { TrackOn, TrackSolo, PluginOn, InsertsOn, SendsOn, SendOn, Pdc, AllEffects, None };

        int id = 0;
        Kind kind = Kind::Toggle;
        Fn   fn   = Fn::PluginOn;
        juce::String name;        // the user's own name, {} = default name
        juce::String trackId;     // te::EditItemID of the track (also the Master track)
        juce::String pluginId;    // plugin itemID           (ParamSlider, PluginOn)
        juce::String paramId;     // parameter id            (ParamSlider)
        juce::String paramName;   // parameter name when it was chosen (fallback when the id is not found)
        int bus = -1;             // send bus                (SendSlider, SendOn)
    };

    std::vector<CtlItem> getItems() const;                       // in the order they were created
    bool hasToggle (CtlItem::Fn, const juce::String& trackId, const juce::String& pluginId, int bus) const;

    int  addParamSlider (const juce::String& trackId, const juce::String& pluginId,
                         const juce::String& paramId, const juce::String& paramName);
    int  addSendSlider  (const juce::String& trackId, int bus);
    int  addTrackSlider (CtlItem::Kind volumeOrPan, const juce::String& trackId);       // -1 = exists already
    bool hasTrackSlider (CtlItem::Kind volumeOrPan, const juce::String& trackId) const;
    int  addToggle      (CtlItem::Fn, const juce::String& trackId, const juce::String& pluginId, int bus);   // -1 = exists already
    int  addItem        (CtlItem item);                          // raw add (the id is given here)
    bool assignItem     (int id, CtlItem target);                // fills an item in place (keeps id + position, drops the old name)
    bool isUnassigned   (int id) const;                          // placeholder with nothing assigned yet
    void removeItem     (int id);
    void renameItem     (int id, const juce::String& newName);   // {} = back to the default name
    juce::String getItemName (int id) const;                     // the name shown on the control

    struct SliderView { bool available = false, unassigned = false; double pos = 0.0; juce::String label, tip; };
    SliderView   getSliderView (int id) const;                   // live state, polled by the panel
    juce::String getSliderText (int id, double pos) const;       // value text for a position 0..1 (the VST's own text)
    void         setSliderPos  (int id, double pos);             // moves the VST parameter (or send) right now
    double       getSliderDefaultPos (int id) const;             // double-click target, -1 = none

    struct ToggleView { bool available = false, on = false, locked = false, unassigned = false; juce::String label, tip; };
    ToggleView getToggleView (int id) const;
    void       setToggleOn   (int id, bool on);

    // Choosing the target (menus of the "+" button)
    struct TrackInfo { juce::String id, name; bool isMaster = false; };
    struct InsertInfo { juce::String id, name; };
    struct FxSendInfo { int bus; juce::String name; };
    struct ParamInfo  { juce::String id, name, text; };
    juce::Array<TrackInfo>  getControlTracks() const;                           // audio tracks in order, then Master
    juce::Array<InsertInfo> getTrackInserts (const juce::String& trackId) const;   // external plugins, chain order
    juce::Array<FxSendInfo> getTrackSends   (const juce::String& trackId) const;   // incl. the Master send
    juce::Array<ParamInfo>  getPluginParams (const juce::String& trackId, const juce::String& pluginId);   // loads the VST if needed
    juce::String getLastParamError() const { return lastParamError; }

    // PDC lives in the audio engine: the window gives the panel its two ends.
    std::function<bool()>     isPdcOn;
    std::function<void (bool)> setPdcOn;

    // The level meter plugin at the end of the Master chain (the same one the mixer's master meter reads;
    // created when it does not exist yet). Used by the control bar's own master meter in the collapsed panel.
    juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> getMasterLevelMeter() const;

    //== Tone / key ======================================================================
    void stepTone (int delta);             // +1 = up one semitone ("NANG"), -1 = down ("HA")
    void stepKeyOffset (int delta);        // KEY - / +
    int  getToneSteps()  const noexcept { return toneSteps; }
    int  getKeyOffset()  const noexcept { return keyOffset; }
    bool canChangeTone() const noexcept { return ! busy; }

    // New song: music back to original pitch, tone + key offset = 0 (the old key stays until the new one arrives).
    void resetShifts();

    // Key from the VPS (or a manual pick): becomes the base key, current offsets are kept.
    void setDetectedKey (const juce::String& root, const juce::String& scale);
    bool hasKey() const noexcept { return baseRoot >= 0; }

    // "C#", "Bb" ... + "Major"/"Minor" after the offsets were applied. {} when there is no key yet.
    juce::String getFinalRootName() const;
    juce::String getScaleName() const noexcept { return baseScale; }

    // Manual pick (used when the VPS failed): root = 0..11
    void setManualKey (int rootSemitone, bool minor);

    //== State shown by the panel ==
    void setBusy (bool b) noexcept { busy = b; }        // true while a song is being analysed (tone buttons locked)
    bool isBusy() const noexcept   { return busy; }

    std::function<void()> onChanged;       // anything the panel shows has changed

    // Diagnostics: log plugins/params of BEAT and MICRO to ptLive.log
    void dumpPlugins() const;

    static int rootSemitone (const juce::String& root);          // -1 = unknown
    static juce::String semitoneName (int semitone, bool preferFlats);
    // Name shown to the user (fixed list): Major = C Db D Eb E F F# G Ab A Bb B,
    // Minor = C C# D Eb E F F# G G# A Bb B.  (semitoneName stays as is: it is used to match plugin parameter text.)
    static juce::String displayRootName (int semitone, bool minor);
    // Same, from a raw root text as sent by the server ("A#" + "minor" -> "Bb"). Unknown root: returned unchanged.
    static juce::String displayRootName (const juce::String& root, const juce::String& scale);
    // Vietnamese name of a key, upper case: ("A","Minor") -> "LA THỨ", ("Bb","Minor") -> "SI GIÁNG THỨ",
    // ("F#","Major") -> "FA THĂNG TRƯỞNG". {} when the root is unknown.
    static juce::String vietnameseKeyName (const juce::String& root, const juce::String& scale);

private:
    te::AudioTrack* findTrack (const juce::String& name) const;
    // Calls fn for every plugin (on any track, any track name) whose name contains `needle`. Returns how many.
    int  forEachPluginNamed (const juce::String& needle,
                             const std::function<void (te::AudioTrack&, te::Plugin&)>& fn) const;
    void logAllTracksPlugins (const juce::String& why) const;
    te::Track*  findTrackById (const juce::String& id) const;
    te::Plugin* findPluginById (te::Track&, const juce::String& id) const;
    te::AuxSendPlugin* findSendOf (te::Track&, int bus) const;
    te::AutomatableParameter::Ptr findParamOf (const CtlItem&) const;
    const CtlItem* findItem (int id) const;
    juce::String defaultNameOf (const CtlItem&) const;
    void loadItems() const;                      // re-reads the session property only when it changed
    void saveItems() const;
    void setInsertsBypassed (te::Track&, bool bypass);
    void setSendsBypassed   (te::Track&, bool bypass);
    void setAllEffectsBypassed (bool bypass);                    // INSERTS + SENDS of every track and the Master
    bool allEffectsActive() const;                               // at least one INSERTS / SENDS section is still active
    void seedPlaceholders() const;                               // 2 empty toggles + 4 empty sliders for a new session
    te::AudioTrack* trackFor (Channel c) const { return findTrack (c == Channel::Beat ? "BEAT" : "MICRO"); }
    te::AutomatableParameter::Ptr findAutotuneKnob (AutotuneKnob k) const;
    void applyToneToPlugin();
    void applyKeyToAutotune();
    void changed() { if (onChanged) onChanged(); }

    TrackManager& tracks;

    int  baseRoot = -1;                    // detected key, 0..11, -1 = none yet
    juce::String baseScale { "Major" };
    int  toneSteps = 0;
    int  keyOffset = 0;
    bool busy = false;
    mutable std::vector<CtlItem> items;          // cache of the session property
    mutable juce::String itemsJson;              // the text `items` was parsed from
    mutable bool itemsLoaded = false;
    juce::String lastParamError;
    mutable bool loggedKnobMissing[2] = { false, false };     // log the plugin's parameter list once, not 7x per second

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveController)
};
