// Self-test for the Net Goal 11 transport crypto (RFC 7748 / FIPS 180-4 vectors).
//
// Build and run with the native build script ("crypto-test" step) or manually:
//   zig c++ -std=c++17 -o crypto_test.exe native/tests/crypto_test.cpp
#include <stdio.h>
#include <string.h>

#include "../common/secure.h"

static int g_failures = 0;

static void Check(const char* name, bool ok) {
    printf("%-42s %s\n", name, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

static bool HexEquals(const uint8_t* data, size_t length, const char* hex) {
    char buffer[160];
    if (length * 2 + 1 > sizeof(buffer)) return false;
    for (size_t i = 0; i < length; ++i) snprintf(buffer + i * 2, 3, "%02x", data[i]);
    buffer[length * 2] = 0;
    return strcmp(buffer, hex) == 0;
}

static void FromHex(uint8_t* out, size_t length, const char* hex) {
    for (size_t i = 0; i < length; ++i) {
        unsigned value = 0;
        sscanf(hex + i * 2, "%2x", &value);
        out[i] = static_cast<uint8_t>(value);
    }
}

int main() {
    // ---- SHA-512 ---------------------------------------------------------
    {
        uint8_t digest[64];
        ngc::Sha512("abc", 3, digest);
        Check("sha512(\"abc\")",
              HexEquals(digest, 64,
                        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    }
    {
        uint8_t digest[64];
        ngc::Sha512("", 0, digest);
        Check("sha512(\"\")",
              HexEquals(digest, 64,
                        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    }

    // ---- X25519 (RFC 7748 section 6.1) -----------------------------------
    {
        uint8_t alicePriv[32], alicePub[32], bobPriv[32], bobPub[32];
        uint8_t aliceShared[32], bobShared[32];
        FromHex(alicePriv, 32,
                "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        FromHex(bobPriv, 32,
                "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
        ngc::X25519Base(alicePub, alicePriv);
        ngc::X25519Base(bobPub, bobPriv);
        Check("x25519 alice public",
              HexEquals(alicePub, 32,
                        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
        Check("x25519 bob public",
              HexEquals(bobPub, 32,
                        "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
        ngc::X25519(aliceShared, alicePriv, bobPub);
        ngc::X25519(bobShared, bobPriv, alicePub);
        Check("x25519 shared secret",
              HexEquals(aliceShared, 32,
                        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
        Check("x25519 both sides agree", memcmp(aliceShared, bobShared, 32) == 0);
    }

    // ---- AEAD round trip + tamper detection ------------------------------
    {
        uint8_t key[32], nonce[24], plain[100], cipher[100], tag[16], decoded[100];
        ngc::RandomBytes(key, sizeof(key));
        ngc::RandomBytes(nonce, sizeof(nonce));
        for (int i = 0; i < 100; ++i) plain[i] = static_cast<uint8_t>(i * 7 + 3);
        ngc::Seal(key, nonce, plain, sizeof(plain), cipher, tag);
        size_t outLen = 0;
        Check("aead open round trip",
              ngc::Open(key, nonce, cipher, sizeof(plain), tag, decoded) &&
                  outLen == 0 && memcmp(plain, decoded, sizeof(plain)) == 0);
        cipher[17] ^= 0x40;
        Check("aead rejects tampered ciphertext",
              !ngc::Open(key, nonce, cipher, sizeof(plain), tag, decoded));
        cipher[17] ^= 0x40;
        tag[3] ^= 1;
        Check("aead rejects tampered tag",
              !ngc::Open(key, nonce, cipher, sizeof(plain), tag, decoded));
    }

    // ---- Channel handshake + data frames ---------------------------------
    {
        ngs::Channel client;
        ngs::Channel server;
        uint8_t init[ngs::kMaxFrame];
        size_t initLen = 0;
        client.BeginClientHandshake(1);
        client.BuildInit(init, initLen);

        uint8_t reply[ngs::kMaxFrame];
        size_t replyLen = 0;
        Check("server accepts init",
              server.HandleInit(init, initLen, reply, replyLen, 2));
        Check("client accepts reply", client.HandleReply(reply, replyLen, 3));

        uint8_t plain[120], frame[ngs::kMaxFrame], out[ngs::kMaxFrame];
        ngc::RandomBytes(plain, sizeof(plain));
        size_t frameLen = 0, outLen = 0;
        Check("client seals", client.Seal(plain, sizeof(plain), frame, frameLen));
        Check("server opens", server.Open(frame, frameLen, out, outLen) &&
                                  outLen == sizeof(plain) &&
                                  memcmp(plain, out, sizeof(plain)) == 0);
        Check("server rejects replay", !server.Open(frame, frameLen, out, outLen));

        size_t frameLen2 = 0;
        Check("server seals", server.Seal(plain, sizeof(plain), frame, frameLen2));
        Check("client opens", client.Open(frame, frameLen2, out, outLen) &&
                                  outLen == sizeof(plain) &&
                                  memcmp(plain, out, sizeof(plain)) == 0);

        // A duplicate init must not invalidate the established keys.
        uint8_t reply2[ngs::kMaxFrame];
        size_t replyLen2 = 0;
        server.HandleInit(init, initLen, reply2, replyLen2, 4);
        Check("repeated init keeps the same server key",
              replyLen2 == replyLen && memcmp(reply, reply2, replyLen) == 0);
    }

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
