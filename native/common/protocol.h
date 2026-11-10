// Net Goal 11 native protocol definitions.
// Wire format is compatible with the original C# protocol for the original
// message types 1..9.  Types 10..11 are native-only probes; type 12 is room chat.
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifndef SIO_UDP_CONNRESET
#    define SIO_UDP_CONNRESET 0x9800000C
#  endif
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

namespace fr {

static constexpr uint32_t kMagic = 0x52313146u; // "F11R" in little-endian
static constexpr uint8_t kVersion = 1;
static constexpr size_t kHeaderSize = 32;
static constexpr size_t kMaxPayload = 1400;
static constexpr size_t kMaxDatagram = kHeaderSize + kMaxPayload;
static constexpr size_t kNameLength = 32;
static constexpr size_t kRoomLength = 16;
static constexpr size_t kMaxPeers = 16;
static constexpr uint16_t kDiscoveryPort = 9999;
static constexpr int kDefaultPort = 45888;
static constexpr int kDefaultGuiPort = 45889;
static constexpr int kDefaultDllPort = 45890;

enum MessageType : uint8_t {
    Hello = 1,
    HelloAck = 2,
    Data = 3,
    Broadcast = 4,
    PeerList = 5,
    Ping = 6,
    Pong = 7,
    Bye = 8,
    Error = 9,
    Probe = 10,
    ProbeAck = 11,
    Chat = 12,
    LobbyCreate = 13,  // GUI -> server: create a lobby room (host)
    LobbyJoin = 14,    // GUI -> server: join an existing lobby room
    LobbyLeave = 15,   // GUI -> server: leave the current lobby room
    LobbyState = 16,   // server -> GUI: room mode + member list
    LobbyStart = 17,   // host GUI -> server -> guest GUIs: launch the game
    LobbyPing = 18,    // GUI -> server: heartbeat / refresh game-path flag
    LobbyBrowse = 19,  // GUI -> server: request the public room list (no membership)
    LobbyList = 20,    // server -> GUI: current public room list
    LobbyKick = 21,    // host GUI -> server: kick one member by slot index
    LobbyChat = 22,    // GUI -> server: room chat text
    LobbyChatMessage = 23, // server -> GUI: formatted room chat / system message
};

static constexpr uint8_t kLobbyFlagHost = 1;
static constexpr uint8_t kLobbyFlagHasGame = 2;
static constexpr uint8_t kMaxLobbyMode = 5;

#pragma pack(push, 1)
struct RelayHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint64_t token;
    uint32_t srcVirt;
    uint32_t dstVirt;
    uint16_t srcPort;
    uint16_t dstPort;
    uint16_t length;
    uint16_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(RelayHeader) == kHeaderSize, "RelayHeader must be 32 bytes");

// IPC between the native GUI and the in-game hook.  Only control/status traffic
// uses this path; game packets go directly from the hook to the relay server.
#pragma pack(push, 1)
struct IpcHeader {
    uint32_t magic;
    uint8_t type;
    uint8_t kind;
    uint16_t flags;
    uint32_t dstVirt;
    uint16_t srcPort;
    uint16_t dstPort;
    uint16_t length;
    uint16_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(IpcHeader) == 20, "IpcHeader must be 20 bytes");

static constexpr uint32_t kIpcMagic = 0x31485246u; // "FRH1"

enum IpcType : uint8_t {
    IpcReady = 1,
    IpcCapture = 2,      // legacy fallback, no longer used in direct mode
    IpcInject = 3,       // legacy fallback
    IpcConfig = 4,
    IpcLog = 5,
    IpcRelayConfig = 6,  // GUI -> hook: (re)connect / update credentials
    IpcStatus = 7,       // hook -> GUI: state + virtual network info + text
    IpcPeers = 8,        // hook -> GUI: PeerList payload
    IpcStats = 9,        // hook -> GUI: four uint64 counters
    IpcRelayReady = 10,  // hook -> GUI: HelloAck received; safe to resume game
    IpcRelayStop = 11,   // GUI -> hook: leave / stop relay
    IpcChat = 12,        // GUI <-> hook: room chat text / relayed chat payload
};

enum RelayState : uint8_t {
    RelayIdle = 0,
    RelayConnecting = 1,
    RelayConnected = 2,
    RelayError = 3,
    RelayDisconnected = 4,
};

#pragma pack(push, 1)
struct IpcStatusPayload {
    uint8_t state;
    uint8_t reserved[3];
    uint32_t selfVirt;
    uint32_t subnet;
    uint32_t mask;
    char text[128];
};

struct IpcPeerPayload {
    uint8_t count;
    // followed by entries: uint32 ip, uint8 nameLen, char name[nameLen]
};

struct IpcRelayConfigPayload {
    uint32_t serverPort;
    uint64_t passwordHash;
    char server[128];
    char room[kRoomLength];
    char name[kNameLength];
};

struct IpcStatsPayload {
    uint64_t upPackets;
    uint64_t downPackets;
    uint64_t upBytes;
    uint64_t downBytes;
};

// Lobby control payloads.  These travel directly between the GUI and the
// relay server, independent of the in-game hook.
struct LobbyRequestPayload {
    uint8_t mode;          // 1..5 for create, ignored for join/ping
    uint8_t hasGame;       // game path configured on this client
    uint16_t reserved;
    uint64_t passwordHash;
    char room[kRoomLength];
    char name[kNameLength];
};

struct LobbyStateHeader {
    uint8_t mode;
    uint8_t memberCount;
    uint8_t selfIsHost;
    uint8_t reserved;
    char room[kRoomLength];
    // followed by entries: uint8 nameLen, uint8 flags, char name[nameLen]
};

struct LobbyStartPayload {
    uint8_t mode;
    uint8_t reserved[3];
};

// Public room list.  The list is polled by every logged-in GUI so it can show
// the room browser before joining anything.
static constexpr int kMaxLobbyListRooms = 16;
static constexpr uint8_t kLobbyListStarted = 1;
static constexpr uint8_t kLobbyListPassword = 2;

struct LobbyListEntry {
    char code[kRoomLength];
    uint8_t mode;              // 1..5
    uint8_t members;           // current member count
    uint8_t capacity;          // mode * 2 (max 10)
    uint8_t flags;             // kLobbyListStarted / kLobbyListPassword
    char host[kNameLength];    // host display name
};

struct LobbyListPayload {
    uint8_t count;
    uint8_t reserved[3];
    LobbyListEntry rooms[kMaxLobbyListRooms];
};

struct LobbyKickPayload {
    uint8_t index;             // slot index in the member list
    uint8_t reserved[3];
};

static constexpr size_t kLobbyChatTextLength = 256;

struct LobbyChatPayload {
    char text[kLobbyChatTextLength]; // UTF-8, NUL terminated
};

struct LobbyChatMessagePayload {
    char time[20];              // "2026-11-04 17:36:20"
    char name[kNameLength];     // empty for system messages
    uint8_t system;             // 1 = join/leave/system message
    uint8_t reserved[3];
    char text[kLobbyChatTextLength];
};
#pragma pack(pop)

static_assert(sizeof(IpcStatusPayload) == 144, "IpcStatusPayload");
static_assert(sizeof(IpcRelayConfigPayload) == 188, "IpcRelayConfigPayload");
static_assert(sizeof(IpcStatsPayload) == 32, "IpcStatsPayload");
static_assert(sizeof(LobbyRequestPayload) == 60, "LobbyRequestPayload");
static_assert(sizeof(LobbyStateHeader) == 20, "LobbyStateHeader");
static_assert(sizeof(LobbyStartPayload) == 4, "LobbyStartPayload");
static_assert(sizeof(LobbyListEntry) == 52, "LobbyListEntry");
static_assert(sizeof(LobbyListPayload) == 4 + 52 * kMaxLobbyListRooms, "LobbyListPayload");
static_assert(sizeof(LobbyKickPayload) == 4, "LobbyKickPayload");
static_assert(sizeof(LobbyChatPayload) == kLobbyChatTextLength, "LobbyChatPayload");
static_assert(sizeof(LobbyChatMessagePayload) == 20 + kNameLength + 4 + kLobbyChatTextLength,
              "LobbyChatMessagePayload");

// All supported targets are little-endian.  memcpy avoids alignment faults.
inline uint16_t Read16(const void* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t Read32(const void* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint64_t Read64(const void* p) { uint64_t v; memcpy(&v, p, 8); return v; }
inline void Write16(void* p, uint16_t v) { memcpy(p, &v, 2); }
inline void Write32(void* p, uint32_t v) { memcpy(p, &v, 4); }
inline void Write64(void* p, uint64_t v) { memcpy(p, &v, 8); }

inline uint32_t IpFromString(const char* text) {
    if (!text || !*text) return 0;
    in_addr a{};
    if (inet_pton(AF_INET, text, &a) != 1) return 0;
    uint32_t v;
    memcpy(&v, &a, 4);
    return v;
}

inline std::string IpToString(uint32_t value) {
    in_addr a{};
    memcpy(&a, &value, 4);
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return std::string(buf);
}

inline uint64_t Fnv1a64(const void* data, size_t length) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < length; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline uint64_t PasswordHash(const std::string& password) {
    if (password.empty()) return 0;
    return Fnv1a64(password.data(), password.size());
}

inline bool EncodeHeader(uint8_t* out, MessageType type, uint64_t token,
                         uint32_t srcVirt, uint32_t dstVirt,
                         uint16_t srcPort, uint16_t dstPort,
                         uint16_t payloadLength, uint16_t flags = 0) {
    if (payloadLength > kMaxPayload) return false;
    RelayHeader h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.type = static_cast<uint8_t>(type);
    h.flags = flags;
    h.token = token;
    h.srcVirt = srcVirt;
    h.dstVirt = dstVirt;
    h.srcPort = srcPort;
    h.dstPort = dstPort;
    h.length = payloadLength;
    h.reserved = 0;
    memcpy(out, &h, sizeof(h));
    return true;
}

inline bool DecodeHeader(const uint8_t* data, size_t size, RelayHeader& h) {
    if (size < kHeaderSize) return false;
    memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.version != kVersion) return false;
    if (h.type < Hello || h.type > LobbyChatMessage) return false;
    if (h.length > kMaxPayload || size < kHeaderSize + h.length) return false;
    return true;
}

// Hello payload: room[16] + passwordHash(8) + name[32]
inline size_t EncodeHello(uint8_t* out, const char* room, uint64_t passwordHash,
                          const std::string& name) {
    memset(out, 0, kRoomLength + 8 + kNameLength);
    strncpy(reinterpret_cast<char*>(out), room ? room : "", kRoomLength);
    Write64(out + kRoomLength, passwordHash);
    size_t copy = name.size();
    if (copy > kNameLength) {
        // Do not split a UTF-8 sequence.
        copy = kNameLength;
        while (copy > 0 && (static_cast<uint8_t>(name[copy]) & 0xC0) == 0x80) --copy;
    }
    memcpy(out + kRoomLength + 8, name.data(), copy);
    return kRoomLength + 8 + kNameLength;
}

inline bool DecodeHello(const uint8_t* payload, size_t length,
                        std::string& room, uint64_t& passwordHash, std::string& name) {
    if (length < kRoomLength + 8 + kNameLength) return false;
    room.assign(reinterpret_cast<const char*>(payload), strnlen(reinterpret_cast<const char*>(payload), kRoomLength));
    passwordHash = Read64(payload + kRoomLength);
    const char* n = reinterpret_cast<const char*>(payload + kRoomLength + 8);
    name.assign(n, strnlen(n, kNameLength));
    return !room.empty();
}

// Peer payload: subnet(4) + mask(4) + room[16] + count(1) + entries
inline size_t EncodeHelloAck(uint8_t* out, uint32_t subnet, uint32_t mask,
                             const char* room, const std::string* names,
                             const uint32_t* ips, int peerCount) {
    size_t off = 0;
    Write32(out + off, subnet); off += 4;
    Write32(out + off, mask); off += 4;
    memset(out + off, 0, kRoomLength);
    strncpy(reinterpret_cast<char*>(out + off), room ? room : "", kRoomLength);
    off += kRoomLength;
    if (peerCount > static_cast<int>(kMaxPeers)) peerCount = static_cast<int>(kMaxPeers);
    out[off++] = static_cast<uint8_t>(peerCount);
    for (int i = 0; i < peerCount; ++i) {
        Write32(out + off, ips[i]); off += 4;
        std::string name = names[i];
        if (name.size() > kNameLength) {
            size_t n = kNameLength;
            while (n > 0 && (static_cast<uint8_t>(name[n]) & 0xC0) == 0x80) --n;
            name.resize(n);
        }
        out[off++] = static_cast<uint8_t>(name.size());
        memcpy(out + off, name.data(), name.size());
        off += name.size();
    }
    return off;
}

inline size_t EncodePeerList(uint8_t* out, const std::string* names,
                             const uint32_t* ips, int peerCount) {
    if (peerCount > static_cast<int>(kMaxPeers)) peerCount = static_cast<int>(kMaxPeers);
    size_t off = 0;
    out[off++] = static_cast<uint8_t>(peerCount);
    for (int i = 0; i < peerCount; ++i) {
        Write32(out + off, ips[i]); off += 4;
        std::string name = names[i];
        if (name.size() > kNameLength) {
            size_t n = kNameLength;
            while (n > 0 && (static_cast<uint8_t>(name[n]) & 0xC0) == 0x80) --n;
            name.resize(n);
        }
        out[off++] = static_cast<uint8_t>(name.size());
        memcpy(out + off, name.data(), name.size());
        off += name.size();
    }
    return off;
}

inline bool DecodePeerList(const uint8_t* payload, size_t length,
                           uint32_t* ips, std::string* names, int& count) {
    count = 0;
    if (length < 1) return false;
    int n = payload[0];
    if (n > static_cast<int>(kMaxPeers)) return false;
    size_t off = 1;
    for (int i = 0; i < n; ++i) {
        if (off + 5 > length) return false;
        ips[i] = Read32(payload + off); off += 4;
        uint8_t nameLen = payload[off++];
        if (nameLen > kNameLength || off + nameLen > length) return false;
        names[i].assign(reinterpret_cast<const char*>(payload + off), nameLen);
        off += nameLen;
    }
    count = n;
    return true;
}

inline bool DecodeHelloAck(const uint8_t* payload, size_t length,
                           uint32_t& subnet, uint32_t& mask, std::string& room,
                           uint32_t* ips, std::string* names, int& count) {
    if (length < 8 + kRoomLength + 1) return false;
    subnet = Read32(payload);
    mask = Read32(payload + 4);
    room.assign(reinterpret_cast<const char*>(payload + 8),
                strnlen(reinterpret_cast<const char*>(payload + 8), kRoomLength));
    return DecodePeerList(payload + 8 + kRoomLength, length - 8 - kRoomLength, ips, names, count);
}

// Chat payload: nameLen(1) + name[nameLen] + UTF-8 text.
static constexpr size_t kMaxChatText = 512;

inline size_t EncodeChat(uint8_t* out, const std::string& name, const std::string& text) {
    size_t nameLen = name.size();
    if (nameLen > 255) nameLen = 255;
    size_t textLen = text.size();
    if (textLen > kMaxChatText) textLen = kMaxChatText;
    out[0] = static_cast<uint8_t>(nameLen);
    memcpy(out + 1, name.data(), nameLen);
    memcpy(out + 1 + nameLen, text.data(), textLen);
    return 1 + nameLen + textLen;
}

inline bool DecodeChat(const uint8_t* payload, size_t length,
                       std::string& name, std::string& text) {
    if (length < 1) return false;
    size_t nameLen = payload[0];
    if (nameLen > 255 || 1 + nameLen > length) return false;
    name.assign(reinterpret_cast<const char*>(payload + 1), nameLen);
    text.assign(reinterpret_cast<const char*>(payload + 1 + nameLen),
                length - 1 - nameLen);
    return true;
}

// Lobby request payload: mode(1) + hasGame(1) + reserved(2) +
// passwordHash(8) + room[16] + name[32].
inline size_t EncodeLobbyRequest(uint8_t* out, uint8_t mode, bool hasGame,
                                 uint64_t passwordHash, const char* room,
                                 const std::string& name) {
    memset(out, 0, sizeof(LobbyRequestPayload));
    out[0] = mode;
    out[1] = hasGame ? 1 : 0;
    Write64(out + 4, passwordHash);
    strncpy(reinterpret_cast<char*>(out + 12), room ? room : "", kRoomLength);
    size_t copy = name.size();
    if (copy > kNameLength) {
        copy = kNameLength;
        while (copy > 0 && (static_cast<uint8_t>(name[copy]) & 0xC0) == 0x80) --copy;
    }
    memcpy(out + 12 + kRoomLength, name.data(), copy);
    return sizeof(LobbyRequestPayload);
}

inline bool DecodeLobbyRequest(const uint8_t* payload, size_t length,
                               uint8_t& mode, bool& hasGame,
                               uint64_t& passwordHash, std::string& room,
                               std::string& name) {
    if (length < sizeof(LobbyRequestPayload)) return false;
    mode = payload[0];
    hasGame = payload[1] != 0;
    passwordHash = Read64(payload + 4);
    room.assign(reinterpret_cast<const char*>(payload + 12),
                strnlen(reinterpret_cast<const char*>(payload + 12), kRoomLength));
    const char* n = reinterpret_cast<const char*>(payload + 12 + kRoomLength);
    name.assign(n, strnlen(n, kNameLength));
    return !room.empty();
}

// Lobby state header + member entries.  Each entry is nameLen(1), flags(1),
// name[nameLen].  flags use kLobbyFlagHost / kLobbyFlagHasGame.
inline size_t EncodeLobbyState(uint8_t* out, uint8_t mode, const char* room,
                               bool selfIsHost, const std::string* names,
                               const uint8_t* flags, int memberCount) {
    if (memberCount < 0) memberCount = 0;
    if (memberCount > static_cast<int>(kMaxPeers)) memberCount = static_cast<int>(kMaxPeers);
    size_t off = 0;
    memset(out, 0, sizeof(LobbyStateHeader));
    out[off++] = mode;
    out[off++] = static_cast<uint8_t>(memberCount);
    out[off++] = selfIsHost ? 1 : 0;
    out[off++] = 0;
    strncpy(reinterpret_cast<char*>(out + off), room ? room : "", kRoomLength);
    off += kRoomLength;
    for (int i = 0; i < memberCount; ++i) {
        std::string name = names[i];
        if (name.size() > kNameLength) {
            size_t n = kNameLength;
            while (n > 0 && (static_cast<uint8_t>(name[n]) & 0xC0) == 0x80) --n;
            name.resize(n);
        }
        out[off++] = static_cast<uint8_t>(name.size());
        out[off++] = flags ? flags[i] : 0;
        memcpy(out + off, name.data(), name.size());
        off += name.size();
    }
    return off;
}

inline bool DecodeLobbyState(const uint8_t* payload, size_t length,
                             uint8_t& mode, bool& selfIsHost,
                             std::string& room, std::string* names,
                             uint8_t* flags, int& memberCount) {
    memberCount = 0;
    if (length < sizeof(LobbyStateHeader)) return false;
    mode = payload[0];
    int count = payload[1];
    selfIsHost = payload[2] != 0;
    room.assign(reinterpret_cast<const char*>(payload + 4),
                strnlen(reinterpret_cast<const char*>(payload + 4), kRoomLength));
    if (count < 0 || count > static_cast<int>(kMaxPeers)) return false;
    size_t off = sizeof(LobbyStateHeader);
    for (int i = 0; i < count; ++i) {
        if (off + 2 > length) return false;
        uint8_t nameLen = payload[off++];
        uint8_t memberFlags = payload[off++];
        if (nameLen > kNameLength || off + nameLen > length) return false;
        names[i].assign(reinterpret_cast<const char*>(payload + off), nameLen);
        flags[i] = memberFlags;
        off += nameLen;
    }
    memberCount = count;
    return true;
}

} // namespace fr
