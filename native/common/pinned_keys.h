// Net Goal 11 - trust-on-first-use store for server encryption keys.
//
// Every relay server owns a long-term X25519 key pair.  The first time a
// client connects to a server it remembers ("pins") the server's public key;
// later connections must present the same key, otherwise the handshake is
// rejected.  That stops an active man-in-the-middle from impersonating the
// relay, and detects a silently replaced server key.
//
// Store format: one "<endpoint> <64 hex chars>" line per server, where
// endpoint is "<ip>:<port>".  On Windows the file lives in
// %LOCALAPPDATA%\Net Goal 11\pinned_servers.txt; the NETGOAL11_PIN_FILE
// environment variable overrides the location (used by the tests).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "crypto.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace ngp {

inline std::string LowerAscii(const std::string& text) {
    std::string out = text;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

// Returns the pin database path, creating the containing directory if needed.
inline std::string StorePath() {
    const char* overridePath = getenv("NETGOAL11_PIN_FILE");
    if (overridePath && *overridePath) return std::string(overridePath);

#ifdef _WIN32
    const char* base = getenv("LOCALAPPDATA");
    std::string dir = base && *base ? std::string(base) : std::string(".");
    dir += "\\Net Goal 11";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\pinned_servers.txt";
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    std::string dir;
    if (xdg && *xdg) dir = std::string(xdg) + "/net-goal-11";
    else if (home && *home) dir = std::string(home) + "/.config/net-goal-11";
    else dir = ".";
    char command[1024];
    snprintf(command, sizeof(command), "mkdir -p '%s' 2>/dev/null", dir.c_str());
    if (system(command) != 0) { /* best effort */ }
    return dir + "/pinned_servers.txt";
#endif
}

inline std::string ToHex(const uint8_t* data, size_t length) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0f]);
    }
    return out;
}

inline bool FromHex(uint8_t* out, size_t length, const char* text) {
    if (strlen(text) < length * 2) return false;
    for (size_t i = 0; i < length; ++i) {
        auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int high = digit(text[i * 2]);
        int low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

// Human-readable fingerprint: SHA-512 of the public key, first 16 bytes,
// upper-case hex in groups of four ("A1B2-C3D4-...").
inline std::string Fingerprint(const uint8_t key[32]) {
    uint8_t hash[64];
    ngc::Sha512(key, 32, hash);
    std::string hex = ToHex(hash, 16);
    for (size_t i = 0; i < hex.size(); ++i) {
        if (hex[i] >= 'a' && hex[i] <= 'f') hex[i] = static_cast<char>(hex[i] - 'a' + 'A');
    }
    std::string out;
    for (size_t i = 0; i < hex.size(); i += 4) {
        if (!out.empty()) out.push_back('-');
        out += hex.substr(i, 4);
    }
    return out;
}

// Looks up the pinned key for `endpoint` ("ip:port").  Returns false when the
// server has never been seen (or the entry is unreadable).
inline bool LoadPin(const std::string& endpoint, uint8_t out[32]) {
    FILE* file = fopen(StorePath().c_str(), "rb");
    if (!file) return false;
    std::string wanted = LowerAscii(endpoint);
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        char* space = strpbrk(line, " \t");
        if (!space) continue;
        std::string key(line, static_cast<size_t>(space - line));
        if (LowerAscii(key) != wanted) continue;
        while (*space == ' ' || *space == '\t') ++space;
        // Trim the trailing newline before parsing the hex payload.
        size_t end = strlen(space);
        while (end > 0 && (space[end - 1] == '\n' || space[end - 1] == '\r')) space[--end] = 0;
        found = FromHex(out, 32, space);
        break;
    }
    fclose(file);
    return found;
}

// Adds or replaces the pinned key for `endpoint` and rewrites the store.
inline bool SavePin(const std::string& endpoint, const uint8_t key[32]) {
    std::string path = StorePath();
    const char* existing = nullptr;
    std::string content;
    if (FILE* file = fopen(path.c_str(), "rb")) {
        char buffer[4096];
        size_t got = 0;
        while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) content.append(buffer, got);
        fclose(file);
        existing = content.c_str();
    }

    std::string wanted = LowerAscii(endpoint);
    std::string output;
    const char* cursor = existing ? existing : "";
    while (*cursor) {
        const char* newline = strchr(cursor, '\n');
        size_t length = newline ? static_cast<size_t>(newline - cursor) : strlen(cursor);
        std::string line(cursor, length);
        if (!line.empty() && line[0] == '#') {
            cursor = newline ? newline + 1 : cursor + length;
            continue; // rewrite the header below so it does not pile up
        }
        if (line.find_first_not_of(" \t\r") != std::string::npos) {
            size_t space = line.find_first_of(" \t");
            std::string key = space == std::string::npos ? line : line.substr(0, space);
            if (LowerAscii(key) == wanted) {
                cursor = newline ? newline + 1 : cursor + length;
                continue; // drop the old entry, it is replaced below
            }
            output += line;
            output += '\n';
        }
        cursor = newline ? newline + 1 : cursor + length;
    }
    output += "# Server encryption keys pinned by Net Goal 11 (trust on first use).\n";
    output += "# Delete a line to trust a reinstalled server again.\n";
    output += LowerAscii(endpoint) + " " + ToHex(key, 32) + "\n";

    // Write to a temporary file first so a crash cannot truncate the store.
    std::string temp = path + ".tmp";
    FILE* file = fopen(temp.c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(output.data(), 1, output.size(), file) == output.size();
    fclose(file);
    if (!ok) {
        remove(temp.c_str());
        return false;
    }
#ifdef _WIN32
    remove(path.c_str());
#endif
    if (rename(temp.c_str(), path.c_str()) != 0) {
        remove(temp.c_str());
        return false;
    }
    return true;
}

} // namespace ngp
