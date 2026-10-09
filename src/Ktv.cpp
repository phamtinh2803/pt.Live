/*
    pt.Live - Live Singing DAW
    KTV (technician) mode - see include/Ktv.h
*/

#include "include/Ktv.h"

#include <cstdint>
#include <string>

namespace ptl::ktv
{
    namespace
    {
        const juce::Identifier kSaltId   ("ptlKtvSalt");
        const juce::Identifier kHashId   ("ptlKtvHash");
        const juce::Identifier kBaseId   ("ptlKtvBase");

        const juce::Identifier* const kAllIds[] = { &kSaltId, &kHashId, &kBaseId };

        // Self-contained SHA-256 (juce::SHA256 lives in the juce_cryptography module, which pt.Live does not link).
        std::string sha256Hex (const std::string& data)
        {
            static const std::uint32_t k[64] = {
                0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
                0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
                0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
                0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
                0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
                0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
                0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
                0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

            std::uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };

            std::string msg = data;
            const std::uint64_t bitLen = (std::uint64_t) data.size() * 8u;
            msg.push_back ((char) 0x80);
            while (msg.size() % 64 != 56)
                msg.push_back ((char) 0);
            for (int i = 7; i >= 0; --i)
                msg.push_back ((char) ((bitLen >> (8 * i)) & 0xff));

            auto rotr = [] (std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

            for (size_t off = 0; off < msg.size(); off += 64)
            {
                std::uint32_t w[64];
                for (int i = 0; i < 16; ++i)
                    w[i] = ((std::uint32_t) (unsigned char) msg[off + (size_t) i * 4]     << 24)
                         | ((std::uint32_t) (unsigned char) msg[off + (size_t) i * 4 + 1] << 16)
                         | ((std::uint32_t) (unsigned char) msg[off + (size_t) i * 4 + 2] << 8)
                         |  (std::uint32_t) (unsigned char) msg[off + (size_t) i * 4 + 3];
                for (int i = 16; i < 64; ++i)
                {
                    const auto s0 = rotr (w[i - 15], 7)  ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const auto s1 = rotr (w[i - 2],  17) ^ rotr (w[i - 2],  19) ^ (w[i - 2]  >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }

                auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
                for (int i = 0; i < 64; ++i)
                {
                    const auto S1  = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
                    const auto ch  = (e & f) ^ (~e & g);
                    const auto t1  = hh + S1 + ch + k[i] + w[i];
                    const auto S0  = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
                    const auto maj = (a & b) ^ (a & c) ^ (b & c);
                    const auto t2  = S0 + maj;
                    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
            }

            static const char* hex = "0123456789abcdef";
            std::string out;
            for (int i = 0; i < 8; ++i)
                for (int sh = 28; sh >= 0; sh -= 4)
                    out.push_back (hex[(h[i] >> sh) & 0xf]);
            return out;
        }

        juce::String hashPassword (const juce::String& salt, const juce::String& password)
        {
            juce::String h = salt + "|" + password;

            for (int i = 0; i < 20000; ++i)                      // slows down guessing a little
            {
                h = juce::String (sha256Hex ((salt + h).toStdString()));
            }

            return h;
        }

        juce::String newSalt()
        {
            auto& r = juce::Random::getSystemRandom();
            return juce::String::toHexString (r.nextInt64()) + juce::String::toHexString (r.nextInt64());
        }
    }

    //==========================================================================
    bool hasKtv (const juce::ValueTree& st)
    {
        return st.getProperty (kHashId).toString().isNotEmpty() && st.getProperty (kSaltId).toString().isNotEmpty();
    }

    bool hasBase (const juce::ValueTree& st)
    {
        return st.getProperty (kBaseId).toString().isNotEmpty();
    }

    bool checkPassword (const juce::ValueTree& st, const juce::String& password)
    {
        if (! hasKtv (st))
            return false;

        return hashPassword (st.getProperty (kSaltId).toString(), password) == st.getProperty (kHashId).toString();
    }

    void setPassword (juce::ValueTree& st, const juce::String& password)
    {
        const auto salt = newSalt();
        st.setProperty (kSaltId, salt, nullptr);
        st.setProperty (kHashId, hashPassword (salt, password), nullptr);
    }

    void clearAll (juce::ValueTree& st)
    {
        for (auto* id : kAllIds)
            st.removeProperty (*id, nullptr);
    }

    //==========================================================================
    bool storeBase (juce::ValueTree& st, const juce::File& savedFile)
    {
        const auto xml = juce::parseXML (savedFile);
        if (xml == nullptr)
            return false;

        auto tree = juce::ValueTree::fromXml (*xml);
        if (! tree.isValid())
            return false;

        for (auto* id : kAllIds)                                  // the copy never contains the KTV data itself
            tree.removeProperty (*id, nullptr);

        juce::MemoryOutputStream packed;
        {
            juce::GZIPCompressorOutputStream gz (packed, 9);
            tree.writeToStream (gz);
            gz.flush();
        }

        st.setProperty (kBaseId, juce::Base64::toBase64 (packed.getData(), packed.getDataSize()), nullptr);
        return true;
    }

    bool writeRestoredFile (const juce::ValueTree& live, const juce::File& target)
    {
        const auto b64 = live.getProperty (kBaseId).toString();
        if (b64.isEmpty())
            return false;

        juce::MemoryOutputStream raw;
        if (! juce::Base64::convertFromBase64 (raw, b64))
            return false;

        auto base = juce::ValueTree::readFromGZIPData (raw.getData(), raw.getDataSize());
        if (! base.isValid())
            return false;

        for (auto* id : kAllIds)                                  // keep the password and the copy itself
            if (live.hasProperty (*id))
                base.setProperty (*id, live.getProperty (*id), nullptr);

        const auto xml = base.createXml();
        if (xml == nullptr)
            return false;

        const auto tmp = target.getSiblingFile (target.getFileName() + ".ktvtmp");
        tmp.deleteFile();

        if (! xml->writeTo (tmp))
        {
            tmp.deleteFile();
            return false;
        }

        if (! tmp.replaceFileIn (target) || target.getSize() <= 0)
        {
            tmp.deleteFile();
            return false;
        }

        return true;
    }

    //==========================================================================
    void askPassword (juce::Component* parent, const juce::String& title, const juce::String& message,
                      bool confirmNew, std::function<void (const juce::String&)> onOk)
    {
        auto* box = new juce::AlertWindow (title, message, juce::MessageBoxIconType::QuestionIcon, parent);
        box->addTextEditor ("pw", {}, "Password", true);

        if (confirmNew)
            box->addTextEditor ("pw2", {}, juce::String::fromUTF8 ("Nhập lại password"), true);

        box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton (juce::String::fromUTF8 ("Huỷ"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<juce::AlertWindow> safeBox (box);
        juce::Component::SafePointer<juce::Component> safeParent (parent);

        box->enterModalState (true, juce::ModalCallbackFunction::create ([safeBox, safeParent, confirmNew, onOk] (int r)
        {
            if (r != 1 || safeBox == nullptr)
                return;

            const auto pw  = safeBox->getTextEditorContents ("pw");
            const auto pw2 = confirmNew ? safeBox->getTextEditorContents ("pw2") : pw;

            if (pw.isEmpty())
            {
                info (safeParent.getComponent(), juce::String::fromUTF8 ("Password không được để trống."), true);
                return;
            }

            if (pw != pw2)
            {
                info (safeParent.getComponent(), juce::String::fromUTF8 ("Hai password không giống nhau."), true);
                return;
            }

            if (onOk)
                onOk (pw);
        }), true);
    }

    void confirm (juce::Component* parent, const juce::String& title, const juce::String& message,
                  const juce::String& okText, std::function<void()> onYes)
    {
        auto* box = new juce::AlertWindow (title, message, juce::MessageBoxIconType::QuestionIcon, parent);
        box->addButton (okText, 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton (juce::String::fromUTF8 ("Huỷ"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

        box->enterModalState (true, juce::ModalCallbackFunction::create ([onYes] (int r)
        {
            if (r == 1 && onYes)
                onYes();
        }), true);
    }

    void info (juce::Component* parent, const juce::String& message, bool warning)
    {
        juce::ignoreUnused (parent);
        juce::AlertWindow::showMessageBoxAsync (warning ? juce::MessageBoxIconType::WarningIcon
                                                        : juce::MessageBoxIconType::InfoIcon,
                                                "pt.Live", message, "OK");
    }
}
