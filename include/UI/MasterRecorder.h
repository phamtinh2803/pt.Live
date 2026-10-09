/*
    pt.Live - Live Singing DAW
    Master Recorder: records the stereo MASTER mix to a WAV file.

    - Only the master bus is recorded, never individual tracks.
    - Folder : Desktop/Record   (created automatically)
    - File   : yyyy-MM-dd_HH-mm-ss_zzz.wav  (e.g. 2024-10-02_14-30-05_123.wav)
             = the recording start time with millisecond precision
    - Format : WAV 24-bit stereo, sample rate = the rate the audio device is currently set to
               (44.1 kHz or 48 kHz...), read when the recording starts.

    Thread safety:
      - pushAudio() runs on the audio thread and NEVER BLOCKS (uses ScopedTryLock).
      - Disk writing happens on a separate TimeSliceThread with a generous 5-second buffer
        so slow disks never starve the audio callback (prevents drop-outs during recording).
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "include/Audio/TracktionHeaders.h"
#include "tracktion_engine/tracktion_engine.h"

namespace ptl {

class MasterRecorder
{
public:
    explicit MasterRecorder (te::Edit&);
    ~MasterRecorder();

    // State queries
    bool isRecording() const noexcept                { return recordingActive.load(); }
    double getDurationSeconds() const noexcept;
    double getRecordingSampleRate() const noexcept   { return recordingSampleRate; }
    juce::File getLastRecordedFile() const noexcept  { return lastRecordedFile; }

    // Folder for the recordings: the one the user chose (File > Recording folder...), else Desktop/Record.
    static juce::File getRecordingsFolder();
    static void setRecordingsFolder (const juce::File& folder);

    // Disk limits: no recording below kMinFreeToStart; a running one is stopped (file closed properly) below kStopFree.
    static constexpr juce::int64 kMinFreeToStart = 2000LL * 1000 * 1000;     // ~2 GB
    static constexpr juce::int64 kStopFree       =  500LL * 1000 * 1000;     // ~500 MB
    static constexpr double      kMaxSegmentBytes = 3.5e9;                   // a WAV holds 4 GB: a new file is started before that

    // Message thread, about every 10 s while recording: stops on low disk space and starts the next
    // file (name_part2.wav ...) before the 4 GB WAV limit.
    enum class ServiceResult { ok, stoppedDiskLow, stoppedError };
    ServiceResult serviceRecording();

    // Set by beginRecording(): something the user should know although recording started
    // (the folder is inside OneDrive). Empty = nothing.
    juce::String getStartWarning() const { return startWarning; }

    // Message thread. Returns false (and fills errorMessage) if recording could not start.
    bool beginRecording (juce::String& errorMessage);
    void stopRecording();                          // flush + close + finalize the WAV header

    // Audio thread (called by TapNode). channels = planar float, numChannels >= 1.
    void pushAudio (const float* const* channels, int numChannels, int numSamples) noexcept;

    // Set by TapNode::prepareToPlay: proves the tap really is part of the running graph.
    void setTapPrepared (bool prepared) noexcept     { tapPrepared.store (prepared); }
    bool isTapPrepared() const noexcept              { return tapPrepared.load(); }

private:
    te::Edit* edit = nullptr;

    bool openWriter (const juce::File& file, double sampleRate,
                     std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>& out, juce::String& error);
    bool rotateFile();                          // closes the current WAV and continues in the next one
    juce::File firstRecordedFile;               // first file of this recording (the parts are named after it)
    int segmentIndex = 1;
    double segmentStartMs = 0.0;
    juce::String startWarning;

    // TimeSliceThread for disk writing - kept alive for the lifetime of MasterRecorder
    // (never stopped between recordings, to avoid the old "2nd recording writes nothing" bug).
    juce::TimeSliceThread writerThread { "pt.Live master recorder" };
    juce::CriticalSection writerLock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;   // guarded by writerLock

    std::atomic<bool> recordingActive { false };
    std::atomic<bool> tapPrepared { false };

    double recordStartMs = 0.0;
    double recordingSampleRate = 0.0;
    juce::File lastRecordedFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterRecorder)
};

//==============================================================================
// Signal statistics measured at the very end of the audio path (what really goes to the sound
// card) and at the driver input. Written on the audio thread, read + logged by AudioEngine.
struct SignalStats
{
    std::atomic<float>         outPeak { 0.0f };   // peak of what is sent to the device (before clamp)
    std::atomic<float>         inPeak  { 0.0f };   // peak of the driver's input channels
    std::atomic<int>           inPeakChannel { -1 };
    std::atomic<unsigned int>  nanCount  { 0 };    // NaN / inf samples replaced by 0
    std::atomic<unsigned int>  overCount { 0 };    // samples beyond +-1.0 that were clamped
    std::atomic<unsigned int>  blocks    { 0 };    // blocks that went through OutputGuardNode
    std::atomic<unsigned int>  xruns     { 0 };    // audio callbacks that took LONGER than the block they had to fill
                                                   // (= certain dropout); cumulative since the app started

    static void raise (std::atomic<float>& a, float v) noexcept
    {
        auto cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }
};

inline SignalStats& signalStats() noexcept
{
    static SignalStats stats;
    return stats;
}

// A startup lock: during the app's first startup phase, prevent any device restarts
// triggered by track routing. The channels were already unioned in applyASIOConfig and opened
// once - this flag just ensures we never reopen during that initial window.
bool isAudioEngineInStartupPhase();  // forward decl, implemented in AudioEngine.cpp

} // namespace ptl

// Custom graph node that taps audio from the master output
namespace tracktion { namespace engine {

/**
    TapNode: passes audio through unchanged while copying it to the MasterRecorder.

    It is wrapped around the master output node via EditPlaybackContext's
    insertOptionalLastStageNodeForDevice callback.
*/
class TapNode final : public graph::Node
{
public:
    TapNode (ptl::MasterRecorder* recorder, std::unique_ptr<graph::Node> inputNode);

    graph::NodeProperties getNodeProperties() override;
    std::vector<graph::Node*> getDirectInputNodes() override;
    bool isReadyToProcess() override;
    TransformResult transform (TransformOptions&) override { return TransformResult::none; }

protected:
    void prepareToPlay (const PlaybackInitialisationInfo&) override;
    void process (ProcessContext&) override;

private:
    ptl::MasterRecorder* masterRecorder = nullptr;
    std::unique_ptr<graph::Node> input;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapNode)
};

/**
    OutputGuardNode: last stage in front of every wave output device. Replaces NaN / inf by 0 and
    clamps to +-1.0 (the sound card cannot play more anyway), and measures the peak. A NaN or an
    exploding value (feedback loop, broken plugin) is what makes a screech that a normal level
    meter does NOT show (comparisons with NaN fail, so the meter stays at 0).
*/
class OutputGuardNode final : public graph::Node
{
public:
    explicit OutputGuardNode (std::unique_ptr<graph::Node> inputNode);

    graph::NodeProperties getNodeProperties() override;
    std::vector<graph::Node*> getDirectInputNodes() override;
    bool isReadyToProcess() override;
    TransformResult transform (TransformOptions&) override { return TransformResult::none; }

protected:
    void prepareToPlay (const PlaybackInitialisationInfo&) override {}
    void process (ProcessContext&) override;

private:
    std::unique_ptr<graph::Node> input;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutputGuardNode)
};

}} // namespace tracktion::engine
