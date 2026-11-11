// Net Goal 11 - encrypted, server-authenticated transport.
//
// Handshake:  X25519 ephemeral (client) + X25519 ephemeral (server) + X25519
//             static server key.  The session key mixes both shared secrets,
//             so only a peer that knows the server's static private key can
//             derive it.  Clients pin the static public key on first use
//             (TOFU), which turns the handshake into real server
//             authentication: an impostor cannot produce a valid confirmation
//             tag.
// Cipher:     XSalsa20-Poly1305 (NaCl secretbox) with per-direction keys,
//             a per-direction nonce prefix and a 64-bit packet counter.
// Framing:    16-byte header (magic, version, type, length, counter) followed
//             by the payload and a 16-byte tag (data frames) or a 16-byte
//             Poly1305 confirmation tag (handshake reply).
#pragma once

#include <string.h>

#include "crypto.h"
#include "protocol.h"

namespace ngs {

static constexpr uint32_t kMagic = 0x3145474Eu; // 'N' 'G' 'E' '1'
static constexpr uint8_t kVersion = 2;

enum FrameType : uint8_t {
    FrameInit = 1,   // client -> server: ephemeral public key + random
    FrameReply = 2,  // server -> client: static key + ephemeral key + random + tag
    FrameData = 3,   // both directions: encrypted payload
};

static constexpr size_t kHeaderSize = 16;
static constexpr size_t kTagSize = 16;
static constexpr size_t kPubSize = 32;
static constexpr size_t kRandomSize = 16;
static constexpr size_t kHandshakePayload = kPubSize + kRandomSize;         // 48
static constexpr size_t kReplyPayload = kPubSize + kPubSize + kRandomSize;   // 80
static constexpr size_t kReplyFrame = kHeaderSize + kReplyPayload + kTagSize;
static constexpr size_t kOverhead = kHeaderSize + kTagSize;                  // 32
static constexpr size_t kMaxPlain = fr::kMaxDatagram;                        // 1432
static constexpr size_t kMaxFrame = kMaxPlain + kOverhead;                   // 1464

inline void WriteFrameHeader(uint8_t* out, uint8_t type, uint16_t length, uint64_t counter) {
    fr::Write32(out, kMagic);
    out[4] = kVersion;
    out[5] = type;
    fr::Write16(out + 6, length);
    fr::Write64(out + 8, counter);
}

inline bool IsSecureFrame(const uint8_t* data, size_t length) {
    return length >= kHeaderSize && fr::Read32(data) == kMagic;
}

inline void MakeNonce(uint8_t nonce[24], const uint8_t prefix[16], uint64_t counter) {
    memcpy(nonce, prefix, 16);
    // Big-endian counter so the two halves of the nonce never overlap in a
    // confusing way; uniqueness is what matters.
    for (int i = 0; i < 8; ++i) nonce[16 + i] = static_cast<uint8_t>(counter >> (56 - 8 * i));
}

// Hashes a fixed-size derivation transcript:
//   label[32] || dhA[32] || dhB[32] || clientPub[32] || serverEphPub[32]
//   || clientRandom[16] || serverRandom[16]
inline void Kdf(const char* label, const uint8_t dhA[32], const uint8_t dhB[32],
                const uint8_t clientPub[32], const uint8_t serverEphPub[32],
                const uint8_t clientRandom[16], const uint8_t serverRandom[16],
                uint8_t out[32]) {
    uint8_t buffer[192];
    memset(buffer, 0, sizeof(buffer));
    size_t n = strlen(label);
    if (n > 32) n = 32;
    memcpy(buffer, label, n);
    memcpy(buffer + 32, dhA, 32);
    memcpy(buffer + 64, dhB, 32);
    memcpy(buffer + 96, clientPub, 32);
    memcpy(buffer + 128, serverEphPub, 32);
    memcpy(buffer + 160, clientRandom, 16);
    memcpy(buffer + 176, serverRandom, 16);
    uint8_t hash[64];
    ngc::Sha512(buffer, sizeof(buffer), hash);
    memcpy(out, hash, 32);
}

struct Channel {
    bool isServer = false;
    bool ready = false;

    // Ephemeral key pair (both roles).
    uint8_t priv[32] = {};
    uint8_t pub[32] = {};
    // Peer's ephemeral public key.
    uint8_t peerPub[32] = {};

    // Server long-term key (server role) and the pinned/learned server key
    // (client role).
    uint8_t staticPriv[32] = {};
    uint8_t staticPub[32] = {};
    uint8_t peerStatic[32] = {};

    uint8_t clientRandom[16] = {};
    uint8_t serverRandom[16] = {};

    uint8_t txKey[32] = {};
    uint8_t rxKey[32] = {};
    uint8_t txPrefix[16] = {};
    uint8_t rxPrefix[16] = {};
    uint8_t confirmKey[32] = {};

    uint64_t txCounter = 0;
    bool rxSeen = false;
    uint64_t rxMax = 0;
    uint64_t rxBitmap = 0;
    uint64_t lastSeenMs = 0;

    void Reset() {
        isServer = false;
        ready = false;
        memset(priv, 0, sizeof(priv));
        memset(pub, 0, sizeof(pub));
        memset(peerPub, 0, sizeof(peerPub));
        memset(staticPriv, 0, sizeof(staticPriv));
        memset(staticPub, 0, sizeof(staticPub));
        memset(peerStatic, 0, sizeof(peerStatic));
        memset(clientRandom, 0, sizeof(clientRandom));
        memset(serverRandom, 0, sizeof(serverRandom));
        memset(txKey, 0, sizeof(txKey));
        memset(rxKey, 0, sizeof(rxKey));
        memset(txPrefix, 0, sizeof(txPrefix));
        memset(rxPrefix, 0, sizeof(rxPrefix));
        memset(confirmKey, 0, sizeof(confirmKey));
        txCounter = 0;
        rxSeen = false;
        rxMax = 0;
        rxBitmap = 0;
        lastSeenMs = 0;
    }

    void Derive() {
        uint8_t dhEphemeral[32] = {};
        uint8_t dhStatic[32] = {};
        if (isServer) {
            ngc::X25519(dhEphemeral, priv, peerPub);        // (server eph, client eph)
            ngc::X25519(dhStatic, staticPriv, peerPub);     // (server static, client eph)
        } else {
            ngc::X25519(dhEphemeral, priv, peerPub);        // (client eph, server eph)
            ngc::X25519(dhStatic, priv, peerStatic);        // (client eph, server static)
        }

        uint8_t c2sKey[32], s2cKey[32], c2sPrefix[32], s2cPrefix[32], confirm[32];
        Kdf("NetGoal11 v2 c2s key", dhEphemeral, dhStatic, ClientPub(), ServerEphPub(),
            clientRandom, serverRandom, c2sKey);
        Kdf("NetGoal11 v2 s2c key", dhEphemeral, dhStatic, ClientPub(), ServerEphPub(),
            clientRandom, serverRandom, s2cKey);
        Kdf("NetGoal11 v2 c2s nonce", dhEphemeral, dhStatic, ClientPub(), ServerEphPub(),
            clientRandom, serverRandom, c2sPrefix);
        Kdf("NetGoal11 v2 s2c nonce", dhEphemeral, dhStatic, ClientPub(), ServerEphPub(),
            clientRandom, serverRandom, s2cPrefix);
        Kdf("NetGoal11 v2 confirm", dhEphemeral, dhStatic, ClientPub(), ServerEphPub(),
            clientRandom, serverRandom, confirm);

        if (isServer) {
            memcpy(txKey, s2cKey, 32);
            memcpy(rxKey, c2sKey, 32);
            memcpy(txPrefix, s2cPrefix, 16);
            memcpy(rxPrefix, c2sPrefix, 16);
        } else {
            memcpy(txKey, c2sKey, 32);
            memcpy(rxKey, s2cKey, 32);
            memcpy(txPrefix, c2sPrefix, 16);
            memcpy(rxPrefix, s2cPrefix, 16);
        }
        memcpy(confirmKey, confirm, 32);
        txCounter = 0;
        rxSeen = false;
        rxMax = 0;
        rxBitmap = 0;
        memset(dhEphemeral, 0, sizeof(dhEphemeral));
        memset(dhStatic, 0, sizeof(dhStatic));
    }

    // Transcript accessors: the client's ephemeral key is `pub` on the client
    // and `peerPub` on the server; the server's ephemeral key is the reverse.
    const uint8_t* ClientPub() const { return isServer ? peerPub : pub; }
    const uint8_t* ServerEphPub() const { return isServer ? pub : peerPub; }

    // ---- client side -------------------------------------------------------
    void BeginClientHandshake(uint64_t nowMs) {
        Reset();
        isServer = false;
        ngc::X25519Keypair(pub, priv);
        ngc::RandomBytes(clientRandom, sizeof(clientRandom));
        lastSeenMs = nowMs;
    }

    bool BuildInit(uint8_t* out, size_t& outLen) const {
        outLen = kHeaderSize + kHandshakePayload;
        WriteFrameHeader(out, FrameInit, static_cast<uint16_t>(kHandshakePayload), 0);
        memcpy(out + kHeaderSize, pub, kPubSize);
        memcpy(out + kHeaderSize + kPubSize, clientRandom, kRandomSize);
        return true;
    }

    // Extracts the server's static public key from a reply frame.  Callers use
    // this to check (or establish) the pinned key before trusting the reply.
    static bool PeekReplyStatic(const uint8_t* frame, size_t length, uint8_t out[32]) {
        if (length != kReplyFrame) return false;
        if (frame[5] != FrameReply) return false;
        memcpy(out, frame + kHeaderSize, kPubSize);
        return true;
    }

    // Verifies the server's confirmation tag and installs the session keys.
    // `expectedStatic` is the pinned server key; when it is not null the reply
    // must carry exactly that key or the handshake is rejected.
    bool HandleReply(const uint8_t* frame, size_t length, const uint8_t* expectedStatic,
                     uint64_t nowMs) {
        if (ready) return false; // a late duplicate reply - keys already set
        if (length != kReplyFrame) return false;
        if (frame[5] != FrameReply) return false;

        const uint8_t* staticPubIn = frame + kHeaderSize;
        const uint8_t* ephPubIn = staticPubIn + kPubSize;
        const uint8_t* random = ephPubIn + kPubSize;
        const uint8_t* tag = random + kRandomSize;

        if (expectedStatic && memcmp(expectedStatic, staticPubIn, kPubSize) != 0) return false;

        memcpy(peerStatic, staticPubIn, kPubSize);
        memcpy(peerPub, ephPubIn, kPubSize);
        memcpy(serverRandom, random, kRandomSize);
        Derive();

        uint8_t expected[kTagSize];
        ngc::Poly1305(expected, frame + kHeaderSize, kReplyPayload, confirmKey);
        if (!ngc::ConstantTimeEqual(expected, tag, kTagSize)) {
            // The peer does not hold the static private key (or the reply was
            // modified in flight): never trust anything derived from it.
            memset(peerStatic, 0, sizeof(peerStatic));
            memset(peerPub, 0, sizeof(peerPub));
            memset(serverRandom, 0, sizeof(serverRandom));
            memset(txKey, 0, sizeof(txKey));
            memset(rxKey, 0, sizeof(rxKey));
            memset(confirmKey, 0, sizeof(confirmKey));
            return false;
        }
        ready = true;
        lastSeenMs = nowMs;
        return true;
    }

    // ---- server side -------------------------------------------------------
    bool HandleInit(const uint8_t* frame, size_t length, const uint8_t staticPrivIn[32],
                    const uint8_t staticPubIn[32], uint8_t* reply, size_t& replyLen,
                    uint64_t nowMs) {
        if (length != kHeaderSize + kHandshakePayload) return false;
        if (frame[5] != FrameInit) return false;
        const uint8_t* clientPub = frame + kHeaderSize;
        const uint8_t* random = clientPub + kPubSize;

        bool samePeer = ready && isServer &&
                        memcmp(staticPub, staticPubIn, kPubSize) == 0 &&
                        memcmp(clientRandom, random, kRandomSize) == 0 &&
                        memcmp(peerPub, clientPub, kPubSize) == 0;
        if (!samePeer) {
            Reset();
            isServer = true;
            memcpy(staticPriv, staticPrivIn, 32);
            memcpy(staticPub, staticPubIn, 32);
            memcpy(peerPub, clientPub, kPubSize);
            memcpy(clientRandom, random, kRandomSize);
            ngc::X25519Keypair(pub, priv);
            ngc::RandomBytes(serverRandom, sizeof(serverRandom));
            Derive();
            ready = true;
        }
        lastSeenMs = nowMs;

        // Reply: static key + ephemeral key + random, authenticated with the
        // confirmation key only the two peers can derive.
        replyLen = kReplyFrame;
        WriteFrameHeader(reply, FrameReply, static_cast<uint16_t>(kReplyPayload), 0);
        memcpy(reply + kHeaderSize, staticPub, kPubSize);
        memcpy(reply + kHeaderSize + kPubSize, pub, kPubSize);
        memcpy(reply + kHeaderSize + kPubSize + kPubSize, serverRandom, kRandomSize);
        ngc::Poly1305(reply + kHeaderSize + kReplyPayload, reply + kHeaderSize, kReplyPayload,
                      confirmKey);
        return true;
    }

    // ---- data frames -------------------------------------------------------
    bool Seal(const uint8_t* plain, size_t length, uint8_t* out, size_t& outLen) {
        if (!ready || length > kMaxPlain) return false;
        outLen = kHeaderSize + length + kTagSize;
        WriteFrameHeader(out, FrameData, static_cast<uint16_t>(length), txCounter);
        uint8_t nonce[24];
        MakeNonce(nonce, txPrefix, txCounter);
        ngc::Seal(txKey, nonce, plain, length, out + kHeaderSize, out + kHeaderSize + length);
        ++txCounter;
        return true;
    }

    bool AcceptCounter(uint64_t counter) {
        if (!rxSeen) {
            rxSeen = true;
            rxMax = counter;
            rxBitmap = 1;
            return true;
        }
        if (counter > rxMax) {
            uint64_t shift = counter - rxMax;
            if (shift >= 64) rxBitmap = 1;
            else rxBitmap = (rxBitmap << shift) | 1ull;
            rxMax = counter;
            return true;
        }
        uint64_t diff = rxMax - counter;
        if (diff >= 64) return false;
        uint64_t bit = 1ull << diff;
        if (rxBitmap & bit) return false;
        rxBitmap |= bit;
        return true;
    }

    bool Open(const uint8_t* frame, size_t length, uint8_t* out, size_t& outLen) {
        if (!ready) return false;
        if (length < kHeaderSize + kTagSize) return false;
        if (frame[5] != FrameData) return false;
        uint16_t plainLength = fr::Read16(frame + 6);
        if (plainLength > kMaxPlain) return false;
        if (length != kHeaderSize + static_cast<size_t>(plainLength) + kTagSize) return false;
        uint64_t counter = fr::Read64(frame + 8);
        if (!AcceptCounter(counter)) return false;
        uint8_t nonce[24];
        MakeNonce(nonce, rxPrefix, counter);
        if (!ngc::Open(rxKey, nonce, frame + kHeaderSize, plainLength,
                       frame + kHeaderSize + plainLength, out)) {
            return false;
        }
        outLen = plainLength;
        return true;
    }
};

} // namespace ngs
