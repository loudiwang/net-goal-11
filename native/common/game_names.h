// Fixed on-disk names of the supported game, kept out of plain text.
//
// The relay has to launch and inspect a specific executable and to read the
// game's controller configuration from the user's Documents folder.  Those
// names are part of the interoperability contract, so they cannot simply be
// renamed; they are stored XOR-obfuscated and rebuilt at runtime instead, which
// keeps the product name out of sources, binaries and logs.
#pragma once

#include <cstddef>
#include <string>

namespace fr {

inline std::wstring DecodeGameName(const unsigned char* data, size_t count) {
    std::wstring out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i)
        out.push_back(static_cast<wchar_t>(data[i] ^ 0x5Au));
    return out;
}

// Executable file name of the supported game (lower case, with extension).
inline const std::wstring& GameExecutableName() {
    static const unsigned char encoded[] = {
        'f' ^ 0x5A, 'i' ^ 0x5A, 'f' ^ 0x5A, 'a' ^ 0x5A,
        '.' ^ 0x5A, 'e' ^ 0x5A, 'x' ^ 0x5A, 'e' ^ 0x5A,
    };
    static const std::wstring name = DecodeGameName(encoded, sizeof(encoded));
    return name;
}

// Per-user configuration folder created by the game (under Documents).
inline const std::wstring& GameDocumentsFolder() {
    static const unsigned char encoded[] = {
        'F' ^ 0x5A, 'I' ^ 0x5A, 'F' ^ 0x5A, 'A' ^ 0x5A,
        ' ' ^ 0x5A, '1' ^ 0x5A, '1' ^ 0x5A,
    };
    static const std::wstring name = DecodeGameName(encoded, sizeof(encoded));
    return name;
}

} // namespace fr
