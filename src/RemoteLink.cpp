// NOT BUILT: this file is not listed in CMakeLists.txt (old code, kept for reference). Delete it if RemoteLink is not coming back.
/*
    pt.Live - Live Singing DAW
    RemoteLink implementation (TCP server, loopback)

    Tất cả thao tác engine/track delegate về callback set bởi MainWindow.
    RemoteLink chỉ lo socket + JSON + thread-safe communication.
*/

#include <JuceHeader.h>
#include "include/RemoteLink.h"
#include "include/Audio/PtLog.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <unistd.h>
#endif

using namespace ptl;

//==============================================================================
namespace
{
    // Một message = 1 dòng JSON (all-on-one-line) + '\n'. JSON::toString tự escape chuỗi.
    juce::String toLine (const juce::var& v)
    {
        return juce::JSON::toString (v, true) + "\n";
    }

    juce::String linkDirName() { return "ptLive"; }

    int currentProcessId()
    {
       #if JUCE_WINDOWS
        return (int) ::GetCurrentProcessId();
       #else
        return (int) ::getpid();
       #endif
    }
}

//==============================================================================
RemoteLink::RemoteLink()
    : Thread ("RemoteLink-Background"),
      token_ (juce::Uuid().toString())   // 32 ký tự hex ngẫu nhiên, mỗi lần chạy một token
{
}

RemoteLink::~RemoteLink()
{
    stop();
}

void RemoteLink::stop()
{
    alive_->store (false);              // vô hiệu hoá các callAsync đang chờ
    stopRequested_.store (true);

    // Đóng socket TRƯỚC để mọi lệnh accept/read đang chờ trả về, rồi mới chờ thread thoát.
    if (listener_ != nullptr) listener_->close();
    if (client_   != nullptr) client_->close();

    stopThread (2000);

    listener_.reset();
    client_.reset();
    connected_.store (false);
    authenticated_.store (false);
    running_.store (false);

    removeLinkFile();
}

bool RemoteLink::start (int initialPort)
{
    if (running_.load()) return false;
    stopRequested_.store (false);
    alive_->store (true);

    int port = initialPort;
    bool opened = false;

    for (int i = 0; i < MAX_PORT_TRIES; ++i, ++port)
    {
        if (openListener (port)) { opened = true; break; }
    }

    if (! opened)
    {
        ptl::diag::write ("RemoteLink: no free port from " + juce::String (initialPort));
        return false;
    }

    currentPort_.store (port);
    running_.store (true);
    writeLinkFile();

    startThread();
    return true;
}

void RemoteLink::setSessionFile (const juce::File& file)
{
    sessionPath_ = file.getFullPathName();
}

void RemoteLink::sendShowCommand (const juce::String& target, bool visible)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("t", "show");
    o->setProperty ("target", target);
    o->setProperty ("visible", visible);
    pushOutgoing (toLine (juce::var (o)));
}

juce::File RemoteLink::getLinkFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile (linkDirName()).getChildFile ("link.json");
}

void RemoteLink::writeLinkFile()
{
    auto dir = getLinkFile().getParentDirectory();
    if (! dir.isDirectory() && ! dir.createDirectory().wasOk()) return;

    // token chỉ gồm ký tự hex nên ghép chuỗi trực tiếp là an toàn.
    const juce::String json = "{\"port\":" + juce::String (getPort())
                            + ",\"pid\":" + juce::String (currentProcessId())
                            + ",\"token\":\"" + token_ + "\"}";
    getLinkFile().replaceWithText (json);
}

void RemoteLink::removeLinkFile()
{
    auto f = getLinkFile();
    if (f.existsAsFile()) f.deleteFile();
}

void RemoteLink::broadcastState (bool music, bool mic, bool reverb, int toneValue,
                                 bool autotuneOn, const juce::String& key, const juce::String& scale)
{
    state_.music = music;
    state_.mic = mic;
    state_.reverb = reverb;
    state_.tone = toneValue;
    state_.autotuneOn = autotuneOn;
    state_.key = key;
    state_.scale = scale;

    auto* o = new juce::DynamicObject();
    o->setProperty ("t", "state");
    o->setProperty ("v", 1);
    o->setProperty ("music", music);
    o->setProperty ("mic", mic);
    o->setProperty ("reverb", reverb);
    o->setProperty ("tone", toneValue);
    o->setProperty ("autotune", autotuneOn);
    o->setProperty ("key", key);
    o->setProperty ("scale", scale);
    pushOutgoing (toLine (juce::var (o)));
}

void RemoteLink::pushOutgoing (const juce::String& json)
{
    const juce::ScopedLock sl (outLock_);
    outQueue_.add (json);
}

void RemoteLink::flushOutgoing()
{
    juce::StringArray pending;
    {
        const juce::ScopedLock sl (outLock_);
        pending.swapWith (outQueue_);
    }

    if (client_ == nullptr || ! client_->isConnected())
        return;                          // không có client -> bỏ message (state sẽ gửi lại khi hello)

    for (auto& line : pending)
    {
        const auto* data = line.toRawUTF8();
        const int   size = (int) line.getNumBytesAsUTF8();
        if (client_->write (data, size) < 0)
            break;
    }
}

void RemoteLink::run()
{
    constexpr int pollMs = 20;

    while (! threadShouldExit() && ! stopRequested_.load())
    {
        // --- Accept (không chặn): chỉ giữ 1 client, client thứ 2 bị đóng ngay ---
        if (listener_ != nullptr && listener_->waitUntilReady (true, connected_.load() ? 0 : pollMs) == 1)
        {
            std::unique_ptr<juce::StreamingSocket> accepted (listener_->waitForNextConnection());

            if (accepted != nullptr)
            {
                if (client_ == nullptr)
                {
                    client_ = std::move (accepted);
                    connected_.store (true);
                    authenticated_.store (false);
                    inBuffer_.clear();
                }
                // else: accepted bị huỷ (đóng) khi ra khỏi scope -> từ chối client thứ 2
            }
        }

        // --- Đọc ---
        if (client_ != nullptr)
        {
            const int ready = client_->waitUntilReady (true, pollMs);

            if (ready < 0)
            {
                dropClient();
            }
            else if (ready > 0)
            {
                char buf[4096];
                const int n = client_->read (buf, (int) sizeof (buf), false);

                if (n <= 0)
                {
                    dropClient();
                }
                else
                {
                    inBuffer_.append (buf, (size_t) n);

                    size_t pos;
                    while ((pos = inBuffer_.find ('\n')) != std::string::npos)
                    {
                        auto line = juce::String::fromUTF8 (inBuffer_.data(), (int) pos).trim();
                        inBuffer_.erase (0, pos + 1);
                        if (line.isNotEmpty())
                            handleIncomingLine (line);
                    }

                    if (inBuffer_.size() > 65536)   // client gửi rác không có '\n'
                        dropClient();
                }
            }
        }

        flushOutgoing();
    }
}

void RemoteLink::dropClient()
{
    client_.reset();
    connected_.store (false);
    authenticated_.store (false);
    inBuffer_.clear();
}

void RemoteLink::handleIncomingLine (const juce::String& line)
{
    auto parsed = juce::JSON::parse (line);
    if (! parsed.isObject()) return;

    const auto type = parsed.getProperty ("t", juce::var()).toString().toLowerCase();
    auto alive = alive_;

    if (type == "hello")
    {
        if (parsed.getProperty ("token", juce::var()).toString() == token_)
        {
            authenticated_.store (true);
            juce::MessageManager::callAsync ([this, alive] { if (alive->load()) sendWelcome(); });
        }
        else
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("t", "error");
            o->setProperty ("msg", "invalid token");
            pushOutgoing (toLine (juce::var (o)));
        }
        return;
    }

    if (! authenticated_.load())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("t", "error");
        o->setProperty ("msg", "hello required");
        pushOutgoing (toLine (juce::var (o)));
        return;
    }

    juce::MessageManager::callAsync ([this, alive, cmd = line] { if (alive->load()) handleCommand (cmd); });
}

void RemoteLink::handleCommand (const juce::String& jsonLine)
{
    auto parsed = juce::JSON::parse (jsonLine);
    if (! parsed.isObject()) return;

    const auto cmd = parsed.getProperty ("t", juce::var()).toString().toLowerCase();

    if (cmd == "set")
    {
        const auto what = parsed.getProperty ("what", juce::var()).toString().toLowerCase();
        const bool on   = (bool) parsed.getProperty ("on", juce::var());

        if      (what == "music")    { if (setMusicMute)       setMusicMute (on); }
        else if (what == "mic")      { if (setMicMute)         setMicMute (on); }
        else if (what == "reverb")   { if (setReverbEnabled)   setReverbEnabled (on); }
        else if (what == "autotune") { if (setAutotuneBypass)  setAutotuneBypass (on); }
        else if (what == "tone")
        {
            const int v = (int) parsed.getProperty ("value", juce::var());
            if (setToneValue) setToneValue (v);
        }
    }
    else if (cmd == "key")
    {
        const auto root  = parsed.getProperty ("root", juce::var()).toString();
        const auto scale = parsed.getProperty ("scale", juce::var()).toString();
        if (root.isNotEmpty() && scale.isNotEmpty() && setKeyScale)
            setKeyScale (root, scale);
    }
    else if (cmd == "show")
    {
        const auto target = parsed.getProperty ("target", juce::var()).toString();
        const bool visible = (bool) parsed.getProperty ("visible", juce::var());
        if (applyWindowVisibility) applyWindowVisibility (target, visible);
    }
    else if (cmd == "dump")
    {
        if (dumpPlugins) dumpPlugins();
    }
    else if (cmd == "quit")
    {
        if (stopRequested_.load()) return;
        if (onQuitRequested) onQuitRequested();
    }
}

void RemoteLink::sendWelcome()
{
    if (! authenticated_.load()) return;

    auto sessionFile = getSessionFile ? getSessionFile() : juce::File();

    auto* st = new juce::DynamicObject();
    st->setProperty ("music", state_.music);
    st->setProperty ("mic", state_.mic);
    st->setProperty ("reverb", state_.reverb);
    st->setProperty ("tone", state_.tone);
    st->setProperty ("autotune", state_.autotuneOn);
    st->setProperty ("key", state_.key);
    st->setProperty ("scale", state_.scale);

    auto* root = new juce::DynamicObject();
    root->setProperty ("t", "welcome");
    root->setProperty ("app", "ptlive");
    root->setProperty ("v", 1);
    if (sessionFile.existsAsFile())      root->setProperty ("session", sessionFile.getFullPathName());
    else if (sessionPath_.isNotEmpty())  root->setProperty ("session", sessionPath_);
    root->setProperty ("state", juce::var (st));

    pushOutgoing (toLine (juce::var (root)));
}

bool RemoteLink::openListener (int port)
{
    listener_.reset();

    auto s = std::make_unique<juce::StreamingSocket>();

    // Chỉ lắng nghe loopback -> máy khác trong LAN không kết nối được.
    if (s->createListener (port, "127.0.0.1"))
    {
        listener_ = std::move (s);
        ptl::diag::write ("RemoteLink listening on 127.0.0.1:" + juce::String (port));
        return true;
    }

    ptl::diag::write ("RemoteLink bind failed on port " + juce::String (port));
    return false;
}
