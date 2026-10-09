/*
    pt.Live - Live Singing DAW
    MasterRecorder & TapNode implementations

    Records the stereo master output to Desktop/Record/<datetime>.wav

    RECORDING THREAD SAFETY:
    - Audio thread: pushAudio() runs in real-time audio callback -> NEVER BLOCKS
      * Uses ScopedTryLock to avoid stalling audio if disk write is slow
      * Drops samples only when absolutely necessary (disk too slow)
    - Disk writer: ThreadedWriter runs on separate worker thread with 2s buffer
      * Writes to disk asynchronously, never called from audio thread
    - Lock contention: minimal due to TryLock approach above
*/

#include "include/UI/MasterRecorder.h"
#include "include/Audio/PtLog.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ptl {

//==============================================================================
MasterRecorder::MasterRecorder (te::Edit& e)
    : edit (&e)
{
}

MasterRecorder::~MasterRecorder()
{
    stopRecording();
}

static juce::File getRecordFolderSetting()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("ptLive").getChildFile ("record_folder.txt");
}

juce::File MasterRecorder::getRecordingsFolder()
{
    // The folder the user picked (File > Recording folder...), if it is still there.
    const auto setting = getRecordFolderSetting();

    if (setting.existsAsFile())
    {
        const juce::File chosen (setting.loadFileAsString().trim());

        if (chosen != juce::File() && chosen.isDirectory())
            return chosen;
    }

    // Default: Desktop/Record - clean, English name for universal compatibility
    return juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
               .getChildFile ("Record");
}

void MasterRecorder::setRecordingsFolder (const juce::File& folder)
{
    const auto setting = getRecordFolderSetting();
    setting.getParentDirectory().createDirectory();
    setting.replaceWithText (folder.getFullPathName());
}

double MasterRecorder::getDurationSeconds() const noexcept
{
    if (! recordingActive.load())
        return 0.0;

    return (juce::Time::getMillisecondCounterHiRes() - recordStartMs) / 1000.0;
}

bool MasterRecorder::beginRecording (juce::String& errorMessage)
{
    if (recordingActive.load())
        return true;

    if (edit == nullptr)
    {
        errorMessage = "Audio engine is not ready.";
        return false;
    }

    // Sample rate = whatever the user set in Devices > audio settings (44.1 kHz / 48 kHz ...).
    const double sr = edit->engine.getDeviceManager().getSampleRate();
    if (sr <= 0.0)
    {
        errorMessage = "No audio device is running.\nOpen Devices and enable at least one output channel.";
        return false;
    }

    if (! tapPrepared.load())
    {
        errorMessage = "The master output is not connected to the recorder yet.\n"
                       "Choose the main output in Devices, then try again.";
        return false;
    }

    // Desktop/Record (or the folder the user chose) - create folder on demand
    const auto dir = getRecordingsFolder();
    const auto dirResult = dir.createDirectory();
    if (dirResult.failed())
    {
        errorMessage = "Could not create the recording folder:\n" + dir.getFullPathName()
                     + "\n\nMake sure you have write permission to this folder.";
        return false;
    }

    // Not enough room for a long recording: refuse instead of failing in the middle of a show.
    // (0 = Windows could not tell: do not block on it.)
    const auto freeBytes = dir.getBytesFreeOnVolume();
    if (freeBytes > 0 && freeBytes < kMinFreeToStart)
    {
        errorMessage = "Not enough free disk space to record.\n\n"
                       "Free: " + juce::File::descriptionOfSizeInBytes (freeBytes)
                     + " (at least 2 GB needed)\nFolder: " + dir.getFullPathName()
                     + "\n\nFree some space or choose another folder (File > Recording folder...).";
        return false;
    }

    startWarning.clear();
    if (dir.getFullPathName().containsIgnoreCase ("OneDrive"))
        startWarning = "The recording folder is inside OneDrive. Syncing can slow the disk and cause dropouts.\n\n"
                       "Choose a local folder in File > Recording folder...";

    // Filename = the recording start time (timer), Windows-safe (no ':'):
    //   yyyy-MM-dd_HH-mm-ss_zzz.wav   e.g. 2024-10-02_14-30-05_123.wav
    const auto now = juce::Time::getCurrentTime();
    const juce::String baseName = now.formatted ("%Y-%m-%d_%H-%M-%S")
                                  + "_" + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);
    auto file = dir.getChildFile (baseName + ".wav").getNonexistentSibling();

    // 24-bit stereo WAV at the device sample rate.
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threaded;
    if (! openWriter (file, sr, threaded, errorMessage))
        return false;

    lastRecordedFile    = file;
    firstRecordedFile   = file;
    segmentIndex        = 1;
    recordingSampleRate = sr;
    recordStartMs       = juce::Time::getMillisecondCounterHiRes();
    segmentStartMs      = recordStartMs;

    {
        const juce::ScopedLock sl (writerLock);
        threadedWriter = std::move (threaded);
        recordingActive.store (true);
    }

    juce::String logMsg = "[REC] Started -> " + lastRecordedFile.getFullPathName()
                      + "  (" + juce::String (sr, 0) + " Hz, 24-bit, "
                      + juce::String (juce::roundToInt (sr * 5.0)) + " smp buffer)";
    DBG(logMsg);
    return true;
}

// Opens `file` as a 24-bit stereo WAV behind a 5-second buffer between the audio thread and the disk thread
// (a short disk stall never reaches the audio callback). Leak-free on failure.
bool MasterRecorder::openWriter (const juce::File& file, double sr,
                                 std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>& out, juce::String& error)
{
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
    {
        error = "Could not create the WAV file:\n" + file.getFullPathName() + "\n\n"
                "Common causes:\n"
                "- File already exists\n"
                "- Disk is full\n"
                "- Permission denied";
        return false;
    }

    juce::WavAudioFormat wavFormat;
    juce::AudioFormatWriter* rawWriter = wavFormat.createWriterFor (stream.get(), sr, 2, 24,
                                                                    juce::StringPairArray(), 0);
    if (rawWriter == nullptr)
    {
        error = "Failed to create the WAV writer (" + juce::String (sr, 0) + " Hz, 24-bit stereo).";
        return false;
    }

    stream.release();   // the writer owns the stream now

    // NOTE: a single TimeSliceThread is reused across recordings; we never call stopThread()
    // after a recording (old bug: stopThread() left it not-running and the 2nd recording wrote nothing).
    if (! writerThread.isThreadRunning())
        writerThread.startThread();

    out = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (rawWriter, writerThread,
                                                                      juce::roundToInt (sr * 5.0));
    return true;
}

// New file without a gap: the next writer is built first, then swapped in under the lock (a pointer
// move); the old one is flushed and finalised afterwards, outside the lock.
bool MasterRecorder::rotateFile()
{
    const auto next = firstRecordedFile
                          .getSiblingFile (firstRecordedFile.getFileNameWithoutExtension()
                                           + "_part" + juce::String (segmentIndex + 1) + ".wav")
                          .getNonexistentSibling();

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> fresh, old;
    juce::String error;

    if (! openWriter (next, recordingSampleRate, fresh, error))
    {
        ptl::diag::write ("[REC] could not start the next file: " + next.getFullPathName());
        return false;
    }

    {
        const juce::ScopedLock sl (writerLock);
        old = std::move (threadedWriter);
        threadedWriter = std::move (fresh);
    }

    old.reset();                                   // flush + finalize the full file

    ++segmentIndex;
    lastRecordedFile = next;
    segmentStartMs   = juce::Time::getMillisecondCounterHiRes();
    ptl::diag::write ("[REC] file size limit -> continuing in " + next.getFileName());
    return true;
}

MasterRecorder::ServiceResult MasterRecorder::serviceRecording()
{
    if (! recordingActive.load())
        return ServiceResult::ok;

    const auto freeBytes = lastRecordedFile.getParentDirectory().getBytesFreeOnVolume();

    if (freeBytes > 0 && freeBytes < kStopFree)
    {
        stopRecording();                           // closes the WAV properly
        ptl::diag::write ("[REC] stopped: disk space low (" + juce::File::descriptionOfSizeInBytes (freeBytes) + " free)");
        return ServiceResult::stoppedDiskLow;
    }

    const double bytesPerSecond = juce::jmax (1.0, recordingSampleRate * 2.0 * 3.0);   // stereo, 24-bit
    const double segmentSeconds = (juce::Time::getMillisecondCounterHiRes() - segmentStartMs) / 1000.0;

    if (segmentSeconds * bytesPerSecond >= kMaxSegmentBytes && ! rotateFile())
    {
        stopRecording();
        ptl::diag::write ("[REC] stopped: could not continue in a new file");
        return ServiceResult::stoppedError;
    }

    return ServiceResult::ok;
}

void MasterRecorder::stopRecording()
{
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> toClose;

    {
        const juce::ScopedLock sl (writerLock);
        recordingActive.store (false);
        toClose = std::move (threadedWriter);
    }

    // Flushes the remaining samples and finalizes the WAV header.
    toClose.reset();

    // NOTE: Do NOT stop the TimeSliceThread after a recording; restarting it for each
    // new recording was causing the "nothing recorded on the 2nd try" bug. The thread
    // stays alive until MasterRecorder is destroyed, so the next beginRecording() can
    // reuse it without issues.
}

void MasterRecorder::pushAudio (const float* const* channels, int numChannels, int numSamples) noexcept
{
    if (numSamples <= 0 || numChannels <= 0 || channels == nullptr)
        return;

    // Audio thread: never block. If stopRecording() holds the lock, this block is simply skipped.
    const juce::ScopedTryLock stl (writerLock);

    if (! stl.isLocked() || threadedWriter == nullptr)
        return;

    const float* stereo[2] { channels[0], numChannels > 1 ? channels[1] : channels[0] };
    threadedWriter->write (stereo, numSamples);
}

} // namespace ptl

//==============================================================================
// TapNode implementation
namespace tracktion { namespace engine {

TapNode::TapNode (ptl::MasterRecorder* recorderIn, std::unique_ptr<graph::Node> inputNode)
    : masterRecorder (recorderIn),
      input (std::move (inputNode))
{
    jassert (masterRecorder != nullptr);
    jassert (input != nullptr);
}

graph::NodeProperties TapNode::getNodeProperties()
{
    // Same audio/latency properties as the node we wrap, only the ID differs.
    graph::NodeProperties props = input != nullptr ? input->getNodeProperties()
                                                   : graph::NodeProperties();
    props.hasAudio = true;
    props.nodeID = 0xeeee1111;

    if (input != nullptr)
        hash_combine (props.nodeID, input->getNodeProperties().nodeID);

    return props;
}

std::vector<graph::Node*> TapNode::getDirectInputNodes()
{
    if (input != nullptr)
        return { input.get() };
    return {};
}

bool TapNode::isReadyToProcess()
{
    if (input != nullptr)
        return input->hasProcessed();
    return false;
}

void TapNode::prepareToPlay (const PlaybackInitialisationInfo&)
{
    if (masterRecorder != nullptr)
        masterRecorder->setTapPrepared (true);
}

void TapNode::process (ProcessContext& pc)
{
    if (input == nullptr)
        return;

    const auto numSamples = static_cast<int> (pc.referenceSampleRange.getLength());
    if (numSamples <= 0)
        return;

    auto out = input->getProcessedOutput();
    auto inAudio = out.audio;

    const int srcChannels = static_cast<int> (inAudio.getNumChannels());
    auto destAudio = pc.buffers.audio;
    const int numDest = static_cast<int> (destAudio.getNumChannels());

    if (numDest <= 0)
        return;

    if (srcChannels <= 0)                            // nothing to pass: silence, not stale memory
    {
        for (int ch = 0; ch < numDest; ++ch)
            if (auto* d = destAudio.getChannel (static_cast<choc::buffer::ChannelCount> (ch)).data.data)
                std::fill (d, d + numSamples, 0.0f);
        return;
    }

    const float* tapPtr[2] { nullptr, nullptr };

    for (int ch = 0; ch < numDest; ++ch)
    {
        auto dstCh = destAudio.getChannel (static_cast<choc::buffer::ChannelCount> (ch));
        auto srcCh = inAudio.getChannel (static_cast<choc::buffer::ChannelCount> (ch < srcChannels ? ch : 0));

        const float* srcData = srcCh.data.data;
        float* dstData = dstCh.data.data;

        if (dstData == nullptr)
            continue;

        if (srcData == nullptr)                      // never leave the device buffer uninitialised
        {
            std::fill (dstData, dstData + numSamples, 0.0f);
            continue;
        }

        // ALWAYS pass the audio through, recording or not.
        for (int s = 0; s < numSamples; ++s)
            dstData[s] = srcData[s];

        if (ch < 2)
            tapPtr[ch] = srcData;
    }

    if (tapPtr[0] == nullptr)
        return;

    if (tapPtr[1] == nullptr)
        tapPtr[1] = tapPtr[0];

    if (masterRecorder != nullptr && masterRecorder->isRecording())
        masterRecorder->pushAudio (tapPtr, 2, numSamples);

    juce::ignoreUnused (pc.buffers.midi);
}

//==============================================================================
// OutputGuardNode implementation
OutputGuardNode::OutputGuardNode (std::unique_ptr<graph::Node> inputNode)
    : input (std::move (inputNode))
{
    jassert (input != nullptr);
}

graph::NodeProperties OutputGuardNode::getNodeProperties()
{
    graph::NodeProperties props = input != nullptr ? input->getNodeProperties()
                                                   : graph::NodeProperties();
    props.hasAudio = true;
    props.nodeID = 0xeeee2222;

    if (input != nullptr)
        hash_combine (props.nodeID, input->getNodeProperties().nodeID);

    return props;
}

std::vector<graph::Node*> OutputGuardNode::getDirectInputNodes()
{
    if (input != nullptr)
        return { input.get() };
    return {};
}

bool OutputGuardNode::isReadyToProcess()
{
    return input != nullptr && input->hasProcessed();
}

void OutputGuardNode::process (ProcessContext& pc)
{
    if (input == nullptr)
        return;

    const auto numSamples = static_cast<int> (pc.referenceSampleRange.getLength());
    auto inAudio   = input->getProcessedOutput().audio;
    auto destAudio = pc.buffers.audio;

    const int srcChannels = static_cast<int> (inAudio.getNumChannels());
    const int numDest     = static_cast<int> (destAudio.getNumChannels());

    if (numSamples <= 0 || numDest <= 0)
        return;

    unsigned int nans = 0, overs = 0;
    float peak = 0.0f;

    for (int ch = 0; ch < numDest; ++ch)
    {
        float* dst = destAudio.getChannel (static_cast<choc::buffer::ChannelCount> (ch)).data.data;
        if (dst == nullptr)
            continue;

        const float* src = srcChannels > 0
                             ? inAudio.getChannel (static_cast<choc::buffer::ChannelCount> (ch < srcChannels ? ch : 0)).data.data
                             : nullptr;

        if (src == nullptr)
        {
            std::fill (dst, dst + numSamples, 0.0f);
            continue;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            float v = src[i];

            // NaN / inf test on the BIT PATTERN (exponent all ones). std::isfinite() is not reliable
            // here: the Release build uses /fp:fast, which lets the compiler assume "no NaN / inf"
            // and drop the check - exactly the protection this node exists for.
            if ((std::bit_cast<std::uint32_t> (v) & 0x7f800000u) == 0x7f800000u)
            {
                v = 0.0f;
                ++nans;
            }
            else
            {
                const float a = std::abs (v);
                if (a > peak) peak = a;

                if (a > 1.0f)
                {
                    v = v > 0.0f ? 1.0f : -1.0f;
                    ++overs;
                }
            }

            dst[i] = v;
        }
    }

    auto& st = ptl::signalStats();
    ptl::SignalStats::raise (st.outPeak, peak);
    if (nans  > 0) st.nanCount.fetch_add  (nans,  std::memory_order_relaxed);
    if (overs > 0) st.overCount.fetch_add (overs, std::memory_order_relaxed);
    st.blocks.fetch_add (1, std::memory_order_relaxed);
}

}} // namespace tracktion::engine
