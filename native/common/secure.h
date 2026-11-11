// Net Goal 11 - encrypted transport between clients/hooks and the relay server.
//
// Handshake:  ephemeral X25519 (client) + ephemeral X25519 (server)
// Cipher:     XSalsa20-Poly1305 (NaCl secretbox) with per-direction keys,
//             a per-direction random nonce prefix and a 64-bit packet counter.
// Framing:    16-byte header (magic, version, type, length, counter) followed
//             by the ciphertext and a 16-byte tag.
//
// The handshake is not authenticated, so it protects against passive
// eavesdropping but not against an active man-in-the-middle.  The goal of this
// layer is to keep lobby and relay traffic unreadable on the wire.
#pragma once

#include <string.h>

#include "crypto.h"
#include "protocol.h"

namespace ngs {

static constexpr uint32_t kMagic = 0x3145474Eu; // 'N' 'G' 'E' '1'
static constexpr uint8_t kVersion = 1;

enum FrameType : uint8_t {
    FrameInit = 1,   // client -> server: ephemeral public key + random
    FrameReply = 2,  // server -> client: ephemeral public key + random
    FrameData = 3,   // both directions: encrypted payload
};

static constexpr size_t kHeaderSize = 16;
static constexpr size_t kTagSize = 16;
static constexpr size_t kPubSize = 32;
static constexpr size_t kRandomSize = 16;
static constexpr size_t kHandshakePayload = kPubSize + kRandomSize; // 48
static constexpr size_t kOverhead = kHeaderSize + kTagSize;         // 32
static constexpr size_t kMaxPlain = fr::kMaxDatagram;               // 1432
static constexpr size_t kMaxFrame = kMaxPlain + kOverhead;          // 1464

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

inline void Kdf(const char* label, const uint8_t shared[32], const uint8_t clientPub[32],
                const uint8_t serverPub[32], const uint8_t clientRandom[16],
                const uint8_t serverRandom[16], uint8_t out[32]) {
    uint8_t buffer[160];
    memset(buffer, 0, sizeof(buffer));
    size_t n = strlen(label);
    if (n > 32) n = 32;
    memcpy(buffer, label, n);
    memcpy(buffer + 32, shared, 32);
    memcpy(buffer + 64, clientPub, 32);
    memcpy(buffer + 96, serverPub, 32);
    memcpy(buffer + 128, clientRandom, 16);
    memcpy(buffer + 144, serverRandom, 16);
    uint8_t hash[64];
    ngc::Sha512(buffer, sizeof(buffer), hash);
    memcpy(out, hash, 32);
}

struct Channel {
    bool isServer = false;
    bool ready = false;

    uint8_t priv[32] = {};
    uint8_t pub[32] = {};
    uint8_t peerPub[32] = {};
    uint8_t clientRandom[16] = {};
    uint8_t serverRandom[16] = {};

    uint8_t txKey[32] = {};
    uint8_t rxKey[32] = {};
    uint8_t txPrefix[16] = {};
    uint8_t rxPrefix[16] = {};

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
        memset(clientRandom, 0, sizeof(clientRandom));
        memset(serverRandom, 0, sizeof(serverRandom));
        memset(txKey, 0, sizeof(txKey));
        memset(rxKey, 0, sizeof(rxKey));
        memset(txPrefix, 0, sizeof(txPrefix));
        memset(rxPrefix, 0, sizeof(rxPrefix));
        txCounter = 0;
        rxSeen = false;
        rxMax = 0;
        rxBitmap = 0;
        lastSeenMs = 0;
    }

    void Derive() {
        uint8_t shared[32] = {};
        ngc::X25519(shared, priv, peerPub);

        uint8_t c2sKey[32], s2cKey[32], c2sPrefix[32], s2cPrefix[32];
        Kdf("NetGoal11 c2s key", shared, peerPubOrClient(), serverPubOrClient(),
            clientRandom, serverRandom, c2sKey);
        Kdf("NetGoal11 s2c key", shared, peerPubOrClient(), serverPubOrClient(),
            clientRandom, serverRandom, s2cKey);
        Kdf("NetGoal11 c2s nonce", shared, peerPubOrClient(), serverPubOrClient(),
            clientRandom, serverRandom, c2sPrefix);
        Kdf("NetGoal11 s2c nonce", shared, peerPubOrClient(), serverPubOrClient(),
            clientRandom, serverRandom, s2cPrefix);

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
        txCounter = 0;
        rxSeen = false;
        rxMax = 0;
        rxBitmap = 0;
        memset(shared, 0, sizeof(shared));
    }

    const uint8_t* peerPubOrClient() const { return isServer ? peerPub : pub; }
    const uint8_t* serverPubOrClient() const { return isServer ? pub : peerPub; }

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

    bool HandleReply(const uint8_t* frame, size_t length, uint64_t nowMs) {
        if (ready) return false; // a late duplicate reply - keys already set
        if (length != kHeaderSize + kHandshakePayload) return false;
        if (frame[5] != FrameReply) return false;
        const uint8_t* serverPub = frame + kHeaderSize;
        const uint8_t* random = serverPub + kPubSize;
        memcpy(peerPub, serverPub, kPubSize);
        memcpy(serverRandom, random, kRandomSize);
        Derive();
        ready = true;
        lastSeenMs = nowMs;
        return true;
    }

    // ---- server side -------------------------------------------------------
    bool HandleInit(const uint8_t* frame, size_t length, uint8_t* reply, size_t& replyLen,
                    uint64_t nowMs) {
        if (length != kHeaderSize + kHandshakePayload) return false;
        if (frame[5] != FrameInit) return false;
        const uint8_t* clientPub = frame + kHeaderSize;
        const uint8_t* random = clientPub + kPubSize;

        bool samePeer = ready && isServer && memcmp(peerPub, clientPub, kPubSize) == 0 &&
                        memcmp(clientRandom, random, kRandomSize) == 0;
        if (!samePeer) {
            Reset();
            isServer = true;
            memcpy(peerPub, clientPub, kPubSize);
            memcpy(clientRandom, random, kRandomSize);
            ngc::X25519Keypair(pub, priv);
            ngc::RandomBytes(serverRandom, sizeof(serverRandom));
            Derive();
            ready = true;
        }
        lastSeenMs = nowMs;

        replyLen = kHeaderSize + kHandshakePayload;
        WriteFrameHeader(reply, FrameReply, static_cast<uint16_t>(kHandshakePayload), 0);
        memcpy(reply + kHeaderSize, pub, kPubSize);
        memcpy(reply + kHeaderSize + kPubSize, serverRandom, kRandomSize);
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
