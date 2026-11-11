// Net Goal 11 - self-contained cryptographic primitives.
//
// All routines here are header-only and portable across the Win32 (x86) and
// Linux (x86_64, musl) targets that Net Goal 11 builds for:
//
//   * SHA-512                    (key derivation)
//   * X25519                     (ephemeral key agreement)
//   * XSalsa20 + Poly1305        (authenticated encryption, a.k.a. NaCl secretbox)
//
// The curve25519 / salsa20 / poly1305 / sha512 code is a C++ port of TweetNaCl
// (public domain, https://tweetnacl.cr.yp.to/).  Only the parts needed for the
// transport are kept.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <stdio.h>
#endif

namespace ngc {

// ---------------------------------------------------------------------------
// Random bytes
// ---------------------------------------------------------------------------
inline bool SystemRandom(void* out, size_t length) {
#ifdef _WIN32
    typedef BOOLEAN(WINAPI * RtlGenRandomFn)(PVOID, ULONG);
    static RtlGenRandomFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE module = LoadLibraryA("advapi32.dll");
        if (module) fn = reinterpret_cast<RtlGenRandomFn>(
            reinterpret_cast<void*>(GetProcAddress(module, "SystemFunction036")));
        resolved = true;
    }
    if (!fn || length == 0) return false;
    return fn(out, static_cast<ULONG>(length)) != FALSE;
#else
    FILE* file = fopen("/dev/urandom", "rb");
    if (!file) return false;
    size_t got = fread(out, 1, length, file);
    fclose(file);
    return got == length;
#endif
}

inline void RandomBytes(void* out, size_t length) {
    if (SystemRandom(out, length)) return;
    // Extremely unlikely fallback: mix a few volatile sources.  Kept only so a
    // missing RNG cannot crash the process.
    static uint64_t state = 0x9E3779B97F4A7C15ull;
    uint8_t* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < length; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        p[i] = static_cast<uint8_t>(state >> 24);
    }
}

inline bool ConstantTimeEqual(const void* a, const void* b, size_t length) {
    const uint8_t* x = static_cast<const uint8_t*>(a);
    const uint8_t* y = static_cast<const uint8_t*>(b);
    uint32_t diff = 0;
    for (size_t i = 0; i < length; ++i) diff |= static_cast<uint32_t>(x[i] ^ y[i]);
    return diff == 0;
}

// ---------------------------------------------------------------------------
// Salsa20 / HSalsa20 / XSalsa20
// ---------------------------------------------------------------------------
namespace detail {

typedef int64_t gf[16];

inline uint32_t Rotl32(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

inline uint32_t Load32(const uint8_t* x) {
    return static_cast<uint32_t>(x[0]) | (static_cast<uint32_t>(x[1]) << 8) |
           (static_cast<uint32_t>(x[2]) << 16) | (static_cast<uint32_t>(x[3]) << 24);
}

inline void Store32(uint8_t* x, uint32_t u) {
    x[0] = static_cast<uint8_t>(u);
    x[1] = static_cast<uint8_t>(u >> 8);
    x[2] = static_cast<uint8_t>(u >> 16);
    x[3] = static_cast<uint8_t>(u >> 24);
}

inline uint64_t Load64(const uint8_t* x) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | x[i];
    return u;
}

inline void Store64(uint8_t* x, uint64_t u) {
    for (int i = 7; i >= 0; --i) { x[i] = static_cast<uint8_t>(u); u >>= 8; }
}

inline const uint8_t kSigma[16] = {'e', 'x', 'p', 'a', 'n', 'd', ' ', '3',
                                   '2', '-', 'b', 'y', 't', 'e', ' ', 'k'};

// The Salsa20 core.  h=1 selects HSalsa20 (used by XSalsa20 key derivation).
inline void SalsaCore(uint8_t* out, const uint8_t* in, const uint8_t* key,
                      const uint8_t* constant, int hsalsa) {
    uint32_t w[16], x[16], y[16], t[4];
    for (int i = 0; i < 4; ++i) {
        x[5 * i] = Load32(constant + 4 * i);
        x[1 + i] = Load32(key + 4 * i);
        x[6 + i] = Load32(in + 4 * i);
        x[11 + i] = Load32(key + 16 + 4 * i);
    }
    for (int i = 0; i < 16; ++i) y[i] = x[i];
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int m = 0; m < 4; ++m) t[m] = x[(5 * j + 4 * m) % 16];
            t[1] ^= Rotl32(t[0] + t[3], 7);
            t[2] ^= Rotl32(t[1] + t[0], 9);
            t[3] ^= Rotl32(t[2] + t[1], 13);
            t[0] ^= Rotl32(t[3] + t[2], 18);
            for (int m = 0; m < 4; ++m) w[4 * j + (j + m) % 4] = t[m];
        }
        for (int m = 0; m < 16; ++m) x[m] = w[m];
    }
    if (hsalsa) {
        for (int i = 0; i < 16; ++i) x[i] += y[i];
        for (int i = 0; i < 4; ++i) {
            x[5 * i] -= Load32(constant + 4 * i);
            x[6 + i] -= Load32(in + 4 * i);
        }
        for (int i = 0; i < 4; ++i) {
            Store32(out + 4 * i, x[5 * i]);
            Store32(out + 16 + 4 * i, x[6 + i]);
        }
    } else {
        for (int i = 0; i < 16; ++i) Store32(out + 4 * i, x[i] + y[i]);
    }
}

// Salsa20 keystream XOR, starting at an arbitrary byte offset (multiple of 64).
inline void Salsa20Xor(uint8_t* out, const uint8_t* in, size_t length,
                       uint64_t counter, const uint8_t nonce[8], const uint8_t key[32]) {
    uint8_t block[64];
    uint8_t z[16];
    memset(z, 0, sizeof(z));
    memcpy(z, nonce, 8);
    Store64(z + 8, counter);
    while (length > 0) {
        SalsaCore(block, z, key, kSigma, 0);
        size_t chunk = length < 64 ? length : 64;
        for (size_t i = 0; i < chunk; ++i) out[i] = static_cast<uint8_t>((in ? in[i] : 0) ^ block[i]);
        out += chunk;
        if (in) in += chunk;
        length -= chunk;
        // Increment the 64-bit block counter (little endian in z[8..15]).
        uint64_t c = Load64(z + 8) + 1;
        Store64(z + 8, c);
    }
}

inline void Hsalsa20(uint8_t out[32], const uint8_t nonce[16], const uint8_t key[32]) {
    SalsaCore(out, nonce, key, kSigma, 1);
}

} // namespace detail

// XSalsa20 keystream XOR: subkey = HSalsa20(nonce[0..15]), stream nonce = nonce[16..23].
inline void Xsalsa20Xor(uint8_t* out, const uint8_t* in, size_t length,
                        size_t skipBytes, const uint8_t nonce[24], const uint8_t key[32]) {
    uint8_t subkey[32];
    detail::Hsalsa20(subkey, nonce, key);

    // Generate the keystream from block 0 and skip the first `skipBytes` bytes.
    uint8_t temp[1664];
    size_t total = skipBytes + length;
    if (total > sizeof(temp)) total = sizeof(temp);
    detail::Salsa20Xor(temp, nullptr, total, 0, nonce + 16, subkey);
    for (size_t i = 0; i < length && skipBytes + i < total; ++i) {
        out[i] = static_cast<uint8_t>((in ? in[i] : 0) ^ temp[skipBytes + i]);
    }
}

// ---------------------------------------------------------------------------
// Poly1305 (NaCl style, 17 x 8-bit limbs - identical to TweetNaCl)
// ---------------------------------------------------------------------------
namespace detail {

inline void Add1305(uint32_t* h, const uint32_t* c) {
    uint32_t u = 0;
    for (int j = 0; j < 17; ++j) {
        u += h[j] + c[j];
        h[j] = u & 255;
        u >>= 8;
    }
}

inline const uint32_t kMinusP[17] = {5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 252};

} // namespace detail

inline void Poly1305(uint8_t out[16], const uint8_t* m, size_t n, const uint8_t key[32]) {
    using namespace detail;
    uint32_t x[17], r[17], h[17], c[17], g[17];
    uint32_t u;
    int i, j;

    for (j = 0; j < 17; ++j) r[j] = h[j] = 0;
    for (j = 0; j < 16; ++j) r[j] = key[j];
    r[3] &= 15;
    r[4] &= 252;
    r[7] &= 15;
    r[8] &= 252;
    r[11] &= 15;
    r[12] &= 252;
    r[15] &= 15;

    while (n > 0) {
        for (j = 0; j < 17; ++j) c[j] = 0;
        for (j = 0; (j < 16) && (j < (int)n); ++j) c[j] = m[j];
        c[j] = 1;
        m += j;
        n -= static_cast<size_t>(j);
        Add1305(h, c);
        for (i = 0; i < 17; ++i) {
            x[i] = 0;
            for (j = 0; j < 17; ++j) {
                x[i] += h[j] * ((j <= i) ? r[i - j] : 320 * r[i + 17 - j]);
            }
        }
        for (i = 0; i < 17; ++i) h[i] = x[i];
        u = 0;
        for (j = 0; j < 16; ++j) {
            u += h[j];
            h[j] = u & 255;
            u >>= 8;
        }
        u += h[16];
        h[16] = u & 3;
        u = 5 * (u >> 2);
        for (j = 0; j < 16; ++j) {
            u += h[j];
            h[j] = u & 255;
            u >>= 8;
        }
        u += h[16];
        h[16] = u;
    }

    for (j = 0; j < 17; ++j) g[j] = h[j];
    Add1305(h, kMinusP);
    uint32_t s = 0u - (h[16] >> 7);
    for (j = 0; j < 17; ++j) h[j] ^= s & (g[j] ^ h[j]);

    for (j = 0; j < 16; ++j) c[j] = key[j + 16];
    c[16] = 0;
    Add1305(h, c);
    for (j = 0; j < 16; ++j) out[j] = static_cast<uint8_t>(h[j]);
}

// ---------------------------------------------------------------------------
// SHA-512
// ---------------------------------------------------------------------------
namespace detail {

inline uint64_t Rotr64(uint64_t x, int c) { return (x >> c) | (x << (64 - c)); }
inline uint64_t Ch64(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (~x & z); }
inline uint64_t Maj64(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint64_t BigSigma0(uint64_t x) { return Rotr64(x, 28) ^ Rotr64(x, 34) ^ Rotr64(x, 39); }
inline uint64_t BigSigma1(uint64_t x) { return Rotr64(x, 14) ^ Rotr64(x, 18) ^ Rotr64(x, 41); }
inline uint64_t SmallSigma0(uint64_t x) { return Rotr64(x, 1) ^ Rotr64(x, 8) ^ (x >> 7); }
inline uint64_t SmallSigma1(uint64_t x) { return Rotr64(x, 19) ^ Rotr64(x, 61) ^ (x >> 6); }

inline const uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

} // namespace detail

inline void Sha512Blocks(uint8_t* x, const uint8_t* m, size_t n) {
    uint64_t z[8], b[8], a[8], w[16], t;
    for (int i = 0; i < 8; ++i) z[i] = a[i] = detail::Load64(x + 8 * i);
    while (n >= 128) {
        for (int i = 0; i < 16; ++i) w[i] = detail::Load64(m + 8 * i);
        for (int i = 0; i < 80; ++i) {
            for (int j = 0; j < 8; ++j) b[j] = a[j];
            t = a[7] + detail::BigSigma1(a[4]) + detail::Ch64(a[4], a[5], a[6]) +
                detail::kSha512K[i] + w[i % 16];
            b[7] = t + detail::BigSigma0(a[0]) + detail::Maj64(a[0], a[1], a[2]);
            b[3] += t;
            for (int j = 0; j < 8; ++j) a[(j + 1) % 8] = b[j];
            if (i % 16 == 15) {
                for (int j = 0; j < 16; ++j) {
                    w[j] += w[(j + 9) % 16] + detail::SmallSigma0(w[(j + 1) % 16]) +
                            detail::SmallSigma1(w[(j + 14) % 16]);
                }
            }
        }
        for (int i = 0; i < 8; ++i) { a[i] += z[i]; z[i] = a[i]; }
        m += 128;
        n -= 128;
    }
    for (int i = 0; i < 8; ++i) detail::Store64(x + 8 * i, z[i]);
}

inline void Sha512(const void* data, size_t length, uint8_t out[64]) {
    static const uint8_t kIv[64] = {
        0x6a, 0x09, 0xe6, 0x67, 0xf3, 0xbc, 0xc9, 0x08, 0xbb, 0x67, 0xae, 0x85, 0x84, 0xca,
        0xa7, 0x3b, 0x3c, 0x6e, 0xf3, 0x72, 0xfe, 0x94, 0xf8, 0x2b, 0xa5, 0x4f, 0xf5, 0x3a,
        0x5f, 0x1d, 0x36, 0xf1, 0x51, 0x0e, 0x52, 0x7f, 0xad, 0xe6, 0x82, 0xd1, 0x9b, 0x05,
        0x68, 0x8c, 0x2b, 0x3e, 0x6c, 0x1f, 0x1f, 0x83, 0xd9, 0xab, 0xfb, 0x41, 0xbd, 0x6b,
        0x5b, 0xe0, 0xcd, 0x19, 0x13, 0x7e, 0x21, 0x79};

    uint8_t h[64], block[256];
    uint64_t bits = static_cast<uint64_t>(length) * 8;
    memcpy(h, kIv, 64);
    const uint8_t* m = static_cast<const uint8_t*>(data);
    Sha512Blocks(h, m, length);
    size_t rest = length & 127;
    m += length - rest;
    memset(block, 0, sizeof(block));
    memcpy(block, m, rest);
    block[rest] = 0x80;
    size_t padded = 256 - 128 * (rest < 112 ? 1 : 0);
    detail::Store64(block + padded - 8, bits);
    Sha512Blocks(h, block, padded);
    memcpy(out, h, 64);
}

// ---------------------------------------------------------------------------
// Curve25519 (X25519) - port of TweetNaCl's crypto_scalarmult
// ---------------------------------------------------------------------------
namespace detail {

inline void Set25519(gf r, const gf a) { for (int i = 0; i < 16; ++i) r[i] = a[i]; }

inline void Car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t(1) << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

inline void Sel25519(gf p, gf q, int b) {
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

inline void Pack25519(uint8_t* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    Car25519(t);
    Car25519(t);
    Car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        Sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<uint8_t>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
    }
}

inline void Unpack25519(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (static_cast<int64_t>(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

inline void FieldAdd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
inline void FieldSub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }

inline void FieldMul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    Car25519(o);
    Car25519(o);
}

inline void FieldSq(gf o, const gf a) { FieldMul(o, a, a); }

inline void Inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        FieldSq(c, c);
        if (a != 2 && a != 4) FieldMul(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

inline uint8_t Par25519(const gf a) {
    uint8_t d[32];
    Pack25519(d, a);
    return d[0] & 1;
}

} // namespace detail

// X25519 scalar multiplication.  Returns false if the result is all zero
// (small-order point), which callers should treat as a failed handshake.
inline bool X25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    using namespace detail;
    static const gf kGf0 = {0};
    static const gf kGf1 = {1};
    static const gf kA24 = {0xDB41, 1};

    uint8_t z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;

    for (int i = 0; i < 31; ++i) z[i] = scalar[i];
    z[31] = static_cast<uint8_t>((scalar[31] & 127) | 64);
    z[0] &= 248;

    Unpack25519(x, point);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        Sel25519(a, b, r);
        Sel25519(c, d, r);
        FieldAdd(e, a, c);
        FieldSub(a, a, c);
        FieldAdd(c, b, d);
        FieldSub(b, b, d);
        FieldSq(d, e);
        FieldSq(f, a);
        FieldMul(a, c, a);
        FieldMul(c, b, e);
        FieldAdd(e, a, c);
        FieldSub(a, a, c);
        FieldSq(b, a);
        FieldSub(c, d, f);
        FieldMul(a, c, kA24);
        FieldAdd(a, a, d);
        FieldMul(c, c, a);
        FieldMul(a, d, f);
        FieldMul(d, b, x);
        FieldSq(b, e);
        Sel25519(a, b, r);
        Sel25519(c, d, r);
    }
    for (int i = 0; i < 16; ++i) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    Inv25519(x + 32, x + 32);
    FieldMul(x + 16, x + 16, x + 32);
    Pack25519(out, x + 16);

    uint8_t zero[32] = {};
    return !ConstantTimeEqual(out, zero, 32);
}

inline bool X25519Base(uint8_t out[32], const uint8_t scalar[32]) {
    static const uint8_t kBasePoint[32] = {9};
    return X25519(out, scalar, kBasePoint);
}

inline void X25519Keypair(uint8_t pub[32], uint8_t priv[32]) {
    RandomBytes(priv, 32);
    X25519Base(pub, priv);
}

// ---------------------------------------------------------------------------
// Authenticated encryption (NaCl secretbox: XSalsa20-Poly1305)
//
// The caller supplies a 24-byte nonce that MUST never repeat for one key.
// `Seal` writes `length` ciphertext bytes plus a 16-byte tag.
// ---------------------------------------------------------------------------
inline void DeriveSubkey(const uint8_t key[32], const char* label, uint8_t out[32]) {
    uint8_t input[32 + 32];
    memcpy(input, label, 32);
    memcpy(input + 32, key, 32);
    uint8_t hash[64];
    Sha512(input, sizeof(input), hash);
    memcpy(out, hash, 32);
}

inline void Seal(const uint8_t key[32], const uint8_t nonce[24], const uint8_t* plain,
                 size_t length, uint8_t* cipher, uint8_t tag[16]) {
    uint8_t macKey[32];
    // First 32 keystream bytes are the Poly1305 key.
    Xsalsa20Xor(macKey, nullptr, 32, 0, nonce, key);
    // Message keystream starts at byte 32.
    Xsalsa20Xor(cipher, plain, length, 32, nonce, key);
    Poly1305(tag, cipher, length, macKey);
}

inline bool Open(const uint8_t key[32], const uint8_t nonce[24], const uint8_t* cipher,
                 size_t length, const uint8_t tag[16], uint8_t* plain) {
    uint8_t macKey[32];
    uint8_t expected[16];
    Xsalsa20Xor(macKey, nullptr, 32, 0, nonce, key);
    Poly1305(expected, cipher, length, macKey);
    if (!ConstantTimeEqual(expected, tag, 16)) return false;
    Xsalsa20Xor(plain, cipher, length, 32, nonce, key);
    return true;
}

} // namespace ngc
