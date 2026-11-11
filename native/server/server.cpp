// Net Goal 11 native UDP relay server.
// Linux: non-blocking UDP socket + epoll, one stack buffer per datagram,
//        no per-packet heap allocation on the forwarding path.
// Windows: select() fallback so the same source can be tested locally.
#define _GNU_SOURCE 1
#include "../common/protocol.h"
#include "../common/pinned_keys.h"
#include "../common/secure.h"
#include "../common/version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <mstcpip.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/epoll.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

using namespace fr;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    int port = kDefaultPort;
    uint32_t subnet = 0;
    uint32_t mask = 0;
    int maxPeers = 16;
    std::string bindAddress = "0.0.0.0";
    std::string keyFile = "net-goal-11-server.key";
};

struct Session {
    uint64_t key = 0;              // (uint64)ip << 16 | port
    sockaddr_in addr{};
    uint64_t token = 0;
    uint32_t virtualIp = 0;
    std::string name;
    std::string room;
    Clock::time_point lastSeen{};
};

struct Room {
    std::string code;
    uint64_t passwordHash = 0;
    std::vector<uint64_t> peers;   // session keys
};

// GUI lobby membership.  This is separate from the relay session above: the
// GUI joins a room before the game starts, then the hook creates the relay session
// with the same room/password when the host presses Start.
struct LobbyClient {
    uint64_t key = 0;
    sockaddr_in addr{};
    std::string room;
    std::string name;
    bool host = false;
    bool hasGame = false;
    Clock::time_point lastSeen{};
};

struct LobbyRoom {
    std::string code;
    uint64_t passwordHash = 0;
    uint8_t mode = 1;              // 1 = 1v1 ... 5 = 5v5
    uint64_t hostKey = 0;
    bool started = false;
    std::vector<uint64_t> members;
};

std::atomic<bool> g_stop{false};
uint32_t g_subnet = 0;
uint32_t g_mask = 0;
uint32_t g_network = 0;
int g_maxPeers = 16;
std::unordered_map<uint64_t, std::unique_ptr<Session>> g_sessions;
std::unordered_map<std::string, Room> g_rooms;
std::unordered_map<uint64_t, LobbyClient> g_lobbyClients;
std::unordered_map<std::string, LobbyRoom> g_lobbyRooms;
// Encrypted transport state, one channel per remote endpoint (both the GUI
// lobby and the in-game hook share the same endpoint key).
std::unordered_map<uint64_t, ngs::Channel> g_channels;

// Long-term server key used to authenticate the handshake.  Clients pin the
// matching public key on first connect (TOFU).
uint8_t g_staticPriv[32] = {};
uint8_t g_staticPub[32] = {};
#ifdef _WIN32
SOCKET g_sock = INVALID_SOCKET;
#else
int g_sock = -1;
#endif

uint64_t EndpointKey(const sockaddr_in& a) {
    return (static_cast<uint64_t>(ntohl(a.sin_addr.s_addr)) << 16) |
           static_cast<uint64_t>(ntohs(a.sin_port));
}

std::string EndpointText(const sockaddr_in& a) {
    return IpToString(a.sin_addr.s_addr) + ":" + std::to_string(ntohs(a.sin_port));
}

uint64_t NewToken() {
    static uint64_t state = 0;
#ifdef _WIN32
    if (state == 0) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        state = static_cast<uint64_t>(q.QuadPart) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
        if (state == 0) state = 0x9E3779B97F4A7C15ull;
    }
#else
    if (state == 0) {
        FILE* f = fopen("/dev/urandom", "rb");
        if (f) { fread(&state, sizeof(state), 1, f); fclose(f); }
        if (state == 0) state = static_cast<uint64_t>(Clock::now().time_since_epoch().count()) ^ 0x9E3779B97F4A7C15ull;
    }
#endif
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state ? state : 0x9E3779B97F4A7C15ull;
}

std::string SanitizeName(std::string name) {
    // Trim ASCII whitespace.
    size_t first = name.find_first_not_of(" \t\r\n");
    size_t last = name.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) name = "player";
    else name = name.substr(first, last - first + 1);
    if (name.size() > kNameLength) {
        size_t n = kNameLength;
        while (n > 0 && (static_cast<uint8_t>(name[n]) & 0xC0) == 0x80) --n;
        name.resize(n);
    }
    return name.empty() ? std::string("player") : name;
}

std::string SanitizeChat(std::string text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    size_t last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    text = text.substr(first, last - first + 1);
    std::string clean;
    clean.reserve(text.size());
    for (unsigned char c : text) {
        if (c == '\r' || c == '\n' || c == '\0') continue;
        if (c < 32 && c != '\t') continue;
        clean.push_back((char)c);
    }
    if (clean.size() > kMaxChatText) clean.resize(kMaxChatText);
    return clean;
}

std::string NormalizeRoom(std::string room) {
    size_t first = room.find_first_not_of(" \t\r\n");
    size_t last = room.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    room = room.substr(first, last - first + 1);
    for (char& c : room) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (room.size() > kRoomLength) room.resize(kRoomLength);
    return room;
}

int PrefixFromMask(uint32_t mask) {
    // The protocol stores IPv4 addresses in little-endian byte order, so the
    // numeric mask has its network bits in the low bytes.  Count set bits;
    // valid masks are contiguous.
    int prefix = 0;
    uint32_t m = mask;
    while (m) { prefix += static_cast<int>(m & 1u); m >>= 1; }
    return prefix;
}

uint32_t AllocateVirtualIp(const Room& room) {
    int prefix = PrefixFromMask(g_mask);
    if (prefix < 8 || prefix > 30) return 0;
    int hostBits = 32 - prefix;
    uint64_t maxHost = 1ull << hostBits;
    uint8_t network[4] = {
        static_cast<uint8_t>(g_network & 0xFF),
        static_cast<uint8_t>((g_network >> 8) & 0xFF),
        static_cast<uint8_t>((g_network >> 16) & 0xFF),
        static_cast<uint8_t>((g_network >> 24) & 0xFF),
    };
    for (uint64_t host = 2; host + 1 < maxHost; ++host) {
        uint8_t candidate[4] = {network[0], network[1], network[2], network[3]};
        for (int i = 0; i < 4; ++i) {
            int shift = (3 - i) * 8;
            candidate[i] = static_cast<uint8_t>(candidate[i] | ((host >> shift) & 0xFF));
        }
        uint32_t value;
        memcpy(&value, candidate, 4);
        bool used = false;
        for (uint64_t key : room.peers) {
            auto it = g_sessions.find(key);
            if (it != g_sessions.end() && it->second->virtualIp == value) { used = true; break; }
        }
        if (!used) return value;
    }
    return 0;
}

void SendRaw(const sockaddr_in& remote, const uint8_t* data, size_t length) {
#ifdef _WIN32
    sendto(g_sock, reinterpret_cast<const char*>(data), static_cast<int>(length), 0,
           reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
#else
    sendto(g_sock, data, length, 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
#endif
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now().time_since_epoch())
                                     .count());
}

// Every datagram sent to a client is encrypted with that endpoint's channel.
void SendTo(const sockaddr_in& remote, const uint8_t* data, size_t length) {
    auto it = g_channels.find(EndpointKey(remote));
    if (it == g_channels.end() || !it->second.ready) return;
    uint8_t frame[ngs::kMaxFrame];
    size_t frameLength = 0;
    if (!it->second.Seal(data, length, frame, frameLength)) return;
    SendRaw(remote, frame, frameLength);
}

void SendMessage(const sockaddr_in& remote, MessageType type, uint64_t token,
                 uint32_t srcVirt, uint32_t dstVirt, uint16_t srcPort,
                 uint16_t dstPort, const uint8_t* payload, uint16_t payloadLength) {
    uint8_t buffer[kMaxDatagram];
    if (!EncodeHeader(buffer, type, token, srcVirt, dstVirt, srcPort, dstPort, payloadLength)) return;
    if (payloadLength) memcpy(buffer + kHeaderSize, payload, payloadLength);
    SendTo(remote, buffer, kHeaderSize + payloadLength);
}

void SendError(const sockaddr_in& remote, const std::string& text) {
    SendMessage(remote, MessageType::Error, 0, 0, 0, 0, 0,
                reinterpret_cast<const uint8_t*>(text.data()), static_cast<uint16_t>(text.size()));
}

struct PeerSnapshot {
    int count = 0;
    uint32_t ips[kMaxPeers] = {};
    std::string names[kMaxPeers];
};

PeerSnapshot SnapshotRoom(const Room& room) {
    PeerSnapshot snap;
    std::vector<Session*> list;
    list.reserve(room.peers.size());
    for (uint64_t key : room.peers) {
        auto it = g_sessions.find(key);
        if (it != g_sessions.end()) list.push_back(it->second.get());
    }
    std::sort(list.begin(), list.end(), [](const Session* a, const Session* b) {
        return a->virtualIp < b->virtualIp;
    });
    for (Session* s : list) {
        if (snap.count >= static_cast<int>(kMaxPeers)) break;
        snap.ips[snap.count] = s->virtualIp;
        snap.names[snap.count] = s->name;
        ++snap.count;
    }
    return snap;
}

void SendHelloAck(const Session& session, const Room& room) {
    PeerSnapshot snap = SnapshotRoom(room);
    uint8_t payload[kMaxDatagram];
    size_t len = EncodeHelloAck(payload, g_subnet, g_mask, room.code.c_str(),
                                snap.names, snap.ips, snap.count);
    SendMessage(session.addr, MessageType::HelloAck, session.token, session.virtualIp, 0,
                0, 0, payload, static_cast<uint16_t>(len));
}

void BroadcastPeerList(const Room& room) {
    PeerSnapshot snap = SnapshotRoom(room);
    uint8_t payload[kMaxDatagram];
    size_t len = EncodePeerList(payload, snap.names, snap.ips, snap.count);
    for (uint64_t key : room.peers) {
        auto it = g_sessions.find(key);
        if (it == g_sessions.end()) continue;
        Session& s = *it->second;
        SendMessage(s.addr, MessageType::PeerList, s.token, 0, 0, 0, 0,
                    payload, static_cast<uint16_t>(len));
    }
}

int LobbyCapacity(uint8_t mode) {
    int capacity = static_cast<int>(mode) * 2;
    if (capacity > g_maxPeers) capacity = g_maxPeers;
    if (capacity < 2) capacity = 2;
    return capacity;
}

void SendLobbyState(const LobbyRoom& room, const LobbyClient& recipient) {
    std::string names[kMaxPeers];
    uint8_t flags[kMaxPeers] = {};
    int count = 0;
    for (uint64_t key : room.members) {
        auto it = g_lobbyClients.find(key);
        if (it == g_lobbyClients.end()) continue;
        if (count >= static_cast<int>(kMaxPeers)) break;
        const LobbyClient& member = it->second;
        names[count] = member.name;
        uint8_t value = 0;
        if (key == room.hostKey) value |= kLobbyFlagHost;
        if (member.hasGame) value |= kLobbyFlagHasGame;
        flags[count] = value;
        ++count;
    }
    uint8_t payload[512];
    size_t length = EncodeLobbyState(payload, room.mode, room.code.c_str(),
                                     recipient.key == room.hostKey,
                                     names, flags, count);
    SendMessage(recipient.addr, MessageType::LobbyState, 0, 0, 0, 0, 0,
                payload, static_cast<uint16_t>(length));
}

void BroadcastLobbyState(const LobbyRoom& room) {
    for (uint64_t key : room.members) {
        auto it = g_lobbyClients.find(key);
        if (it != g_lobbyClients.end()) SendLobbyState(room, it->second);
    }
}

std::string CurrentTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm);
    return text;
}

std::string SanitizeChatText(const char* raw) {
    std::string text;
    if (!raw) return text;
    for (size_t i = 0; i < kLobbyChatTextLength && raw[i]; ++i) {
        unsigned char ch = static_cast<unsigned char>(raw[i]);
        if (ch == '\r' || ch == '\n' || ch == '\t') text.push_back(' ');
        else if (ch >= 32) text.push_back(static_cast<char>(ch));
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return text.substr(first);
}

void SendLobbyChatMessage(const LobbyRoom& room, const std::string& name,
                          const std::string& text, bool system) {
    LobbyChatMessagePayload payload{};
    std::string time = CurrentTimestamp();
    memcpy(payload.time, time.c_str(), std::min(time.size(), sizeof(payload.time) - 1));
    if (!name.empty()) {
        memcpy(payload.name, name.c_str(), std::min(name.size(), sizeof(payload.name) - 1));
    }
    payload.system = system ? 1 : 0;
    memcpy(payload.text, text.c_str(), std::min(text.size(), sizeof(payload.text) - 1));
    for (uint64_t key : room.members) {
        auto it = g_lobbyClients.find(key);
        if (it == g_lobbyClients.end()) continue;
        SendMessage(it->second.addr, MessageType::LobbyChatMessage, 0, 0, 0, 0, 0,
                    reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    }
}

void HandleLobbyBrowse(const sockaddr_in& remote) {
    LobbyListPayload payload{};
    int count = 0;
    for (const auto& entry : g_lobbyRooms) {
        if (count >= kMaxLobbyListRooms) break;
        const LobbyRoom& room = entry.second;
        LobbyListEntry& out = payload.rooms[count];
        memset(&out, 0, sizeof(out));
        strncpy(out.code, room.code.c_str(), kRoomLength);
        out.mode = room.mode;
        size_t members = room.members.size();
        out.members = static_cast<uint8_t>(members > 255 ? 255 : members);
        out.capacity = static_cast<uint8_t>(LobbyCapacity(room.mode));
        uint8_t flags = 0;
        if (room.started) flags |= kLobbyListStarted;
        if (room.passwordHash != 0) flags |= kLobbyListPassword;
        out.flags = flags;
        auto hostIt = g_lobbyClients.find(room.hostKey);
        if (hostIt != g_lobbyClients.end()) {
            strncpy(out.host, hostIt->second.name.c_str(), kNameLength);
        }
        ++count;
    }
    payload.count = static_cast<uint8_t>(count);
    SendMessage(remote, MessageType::LobbyList, 0, 0, 0, 0, 0,
                reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
}

void RemoveLobbyClient(uint64_t key, const char* reason) {
    auto it = g_lobbyClients.find(key);
    if (it == g_lobbyClients.end()) return;
    LobbyClient client = it->second;
    g_lobbyClients.erase(it);

    auto roomIt = g_lobbyRooms.find(client.room);
    if (roomIt == g_lobbyRooms.end()) return;
    LobbyRoom& room = roomIt->second;
    printf("[lobby] %s leave %s room=%s (%s)\n", client.name.c_str(),
           EndpointText(client.addr).c_str(), room.code.c_str(), reason);

    if (client.host) {
        for (uint64_t memberKey : room.members) {
            if (memberKey == key) continue;
            auto memberIt = g_lobbyClients.find(memberKey);
            if (memberIt == g_lobbyClients.end()) continue;
            SendError(memberIt->second.addr, "The host left; the room was closed");
            g_lobbyClients.erase(memberIt);
        }
        printf("[lobby] %s closed by host\n", room.code.c_str());
        g_lobbyRooms.erase(roomIt);
        return;
    }

    room.members.erase(std::remove(room.members.begin(), room.members.end(), key),
                       room.members.end());
    if (room.members.empty()) {
        printf("[lobby] %s removed\n", room.code.c_str());
        g_lobbyRooms.erase(roomIt);
    } else {
        BroadcastLobbyState(room);
    }
}

void HandleLobbyCreate(const uint8_t* data, const sockaddr_in& remote,
                       const RelayHeader& header) {
    uint8_t mode = 0;
    bool hasGame = false;
    uint64_t passwordHash = 0;
    std::string roomCode, name;
    if (!DecodeLobbyRequest(data + kHeaderSize, header.length, mode, hasGame,
                            passwordHash, roomCode, name)) {
        SendError(remote, "bad lobby create");
        return;
    }
    roomCode = NormalizeRoom(roomCode);
    name = SanitizeName(name);
    if (roomCode.empty()) { SendError(remote, "Room code cannot be empty"); return; }
    if (mode < 1 || mode > kMaxLobbyMode) { SendError(remote, "Invalid match mode"); return; }

    uint64_t key = EndpointKey(remote);
    auto existingIt = g_lobbyClients.find(key);
    if (existingIt != g_lobbyClients.end()) {
        LobbyClient& existing = existingIt->second;
        if (existing.host && existing.room == roomCode) {
            // Retry of a create whose reply was lost: refresh and resend.
            existing.name = name;
            existing.hasGame = hasGame;
            existing.lastSeen = Clock::now();
            auto roomIt = g_lobbyRooms.find(roomCode);
            if (roomIt != g_lobbyRooms.end()) SendLobbyState(roomIt->second, existing);
            return;
        }
        RemoveLobbyClient(key, "switch room");
    }

    auto roomIt = g_lobbyRooms.find(roomCode);
    if (roomIt != g_lobbyRooms.end()) {
        SendError(remote, "Room already exists; join it or choose another code");
        return;
    }

    LobbyRoom room;
    room.code = roomCode;
    room.passwordHash = passwordHash;
    room.mode = mode;
    room.hostKey = key;
    room.started = false;
    roomIt = g_lobbyRooms.emplace(roomCode, std::move(room)).first;

    LobbyClient client;
    client.key = key;
    client.addr = remote;
    client.room = roomCode;
    client.name = name;
    client.host = true;
    client.hasGame = hasGame;
    client.lastSeen = Clock::now();
    g_lobbyClients.emplace(key, std::move(client));

    roomIt->second.members.push_back(key);
    printf("[lobby] %s created by %s mode=%u players=1\n", roomCode.c_str(),
           EndpointText(remote).c_str(), static_cast<unsigned>(mode));
    SendLobbyState(roomIt->second, g_lobbyClients[key]);
}

void HandleLobbyJoin(const uint8_t* data, const sockaddr_in& remote,
                     const RelayHeader& header) {
    uint8_t mode = 0;
    bool hasGame = false;
    uint64_t passwordHash = 0;
    std::string roomCode, name;
    if (!DecodeLobbyRequest(data + kHeaderSize, header.length, mode, hasGame,
                            passwordHash, roomCode, name)) {
        SendError(remote, "bad lobby join");
        return;
    }
    roomCode = NormalizeRoom(roomCode);
    name = SanitizeName(name);
    if (roomCode.empty()) { SendError(remote, "Room code cannot be empty"); return; }

    auto roomIt = g_lobbyRooms.find(roomCode);
    if (roomIt == g_lobbyRooms.end()) { SendError(remote, "Room not found"); return; }
    LobbyRoom& room = roomIt->second;
    if (room.passwordHash != passwordHash) { SendError(remote, "Wrong room password"); return; }
    if (room.started) { SendError(remote, "The match has already started"); return; }

    uint64_t key = EndpointKey(remote);
    auto existingIt = g_lobbyClients.find(key);
    if (existingIt != g_lobbyClients.end()) {
        LobbyClient& existing = existingIt->second;
        if (existing.room == roomCode) {
            existing.name = name;
            existing.hasGame = hasGame;
            existing.lastSeen = Clock::now();
            SendLobbyState(room, existing);
            return;
        }
        RemoveLobbyClient(key, "switch room");
    }

    if (static_cast<int>(room.members.size()) >= LobbyCapacity(room.mode)) {
        SendError(remote, "Room is full");
        return;
    }

    LobbyClient client;
    client.key = key;
    client.addr = remote;
    client.room = roomCode;
    client.name = name;
    client.host = false;
    client.hasGame = hasGame;
    client.lastSeen = Clock::now();
    g_lobbyClients.emplace(key, std::move(client));
    room.members.push_back(key);
    printf("[lobby] %s joined %s room=%s players=%zu\n", name.c_str(),
           EndpointText(remote).c_str(), roomCode.c_str(), room.members.size());
    BroadcastLobbyState(room);
    SendLobbyChatMessage(room, "", "Welcome " + name + " to room " + roomCode + ".", true);
}

void HandleLobbyPing(const uint8_t* data, const sockaddr_in& remote,
                     const RelayHeader& header) {
    uint8_t mode = 0;
    bool hasGame = false;
    uint64_t passwordHash = 0;
    std::string roomCode, name;
    if (!DecodeLobbyRequest(data + kHeaderSize, header.length, mode, hasGame,
                            passwordHash, roomCode, name)) {
        return;
    }
    uint64_t key = EndpointKey(remote);
    auto it = g_lobbyClients.find(key);
    if (it == g_lobbyClients.end()) {
        SendError(remote, "lobby session expired");
        return;
    }
    LobbyClient& client = it->second;
    auto roomIt = g_lobbyRooms.find(client.room);
    if (roomIt == g_lobbyRooms.end()) {
        g_lobbyClients.erase(it);
        SendError(remote, "Room closed");
        return;
    }
    bool changed = client.hasGame != hasGame || client.name != SanitizeName(name);
    client.name = SanitizeName(name);
    client.hasGame = hasGame;
    client.lastSeen = Clock::now();
    if (changed) BroadcastLobbyState(roomIt->second);
}

void HandleLobbyLeave(const sockaddr_in& remote) {
    RemoveLobbyClient(EndpointKey(remote), "leave");
}

void HandleLobbyKick(const uint8_t* data, size_t size, const sockaddr_in& remote,
                     const RelayHeader& header) {
    if (header.length < sizeof(LobbyKickPayload) || size < kHeaderSize + sizeof(LobbyKickPayload)) {
        SendError(remote, "bad lobby kick");
        return;
    }
    const auto* payload = reinterpret_cast<const LobbyKickPayload*>(data + kHeaderSize);
    uint64_t key = EndpointKey(remote);
    auto it = g_lobbyClients.find(key);
    if (it == g_lobbyClients.end()) { SendError(remote, "Not in a room"); return; }
    LobbyClient& client = it->second;
    auto roomIt = g_lobbyRooms.find(client.room);
    if (roomIt == g_lobbyRooms.end()) { SendError(remote, "Room not found"); return; }
    LobbyRoom& room = roomIt->second;
    if (!client.host || room.hostKey != key) {
        SendError(remote, "Only the host can kick players");
        return;
    }
    if (payload->index >= room.members.size()) return;
    uint64_t targetKey = room.members[payload->index];
    if (targetKey == key) return;
    auto targetIt = g_lobbyClients.find(targetKey);
    if (targetIt == g_lobbyClients.end()) {
        room.members.erase(room.members.begin() + payload->index);
        BroadcastLobbyState(room);
        return;
    }
    printf("[lobby] %s kicked %s from room=%s\n", client.name.c_str(),
           targetIt->second.name.c_str(), room.code.c_str());
    SendError(targetIt->second.addr, "You were removed from the room by the host");
    g_lobbyClients.erase(targetIt);
    room.members.erase(room.members.begin() + payload->index);
    if (room.members.empty()) {
        g_lobbyRooms.erase(roomIt);
    } else {
        BroadcastLobbyState(room);
    }
}

void HandleLobbyChat(const uint8_t* data, size_t size, const sockaddr_in& remote,
                     const RelayHeader& header) {
    if (header.length < sizeof(LobbyChatPayload) ||
        size < kHeaderSize + sizeof(LobbyChatPayload)) {
        SendError(remote, "bad lobby chat");
        return;
    }
    auto it = g_lobbyClients.find(EndpointKey(remote));
    if (it == g_lobbyClients.end()) { SendError(remote, "Not in a room"); return; }
    LobbyClient& client = it->second;
    auto roomIt = g_lobbyRooms.find(client.room);
    if (roomIt == g_lobbyRooms.end()) { SendError(remote, "Room not found"); return; }
    const auto* payload = reinterpret_cast<const LobbyChatPayload*>(data + kHeaderSize);
    std::string text = SanitizeChatText(payload->text);
    if (text.empty()) return;
    SendLobbyChatMessage(roomIt->second, client.name, text, false);
}

void HandleLobbyStart(const uint8_t* data, size_t size, const sockaddr_in& remote,
                      const RelayHeader& header) {
    (void)data;
    (void)size;
    (void)header;
    uint64_t key = EndpointKey(remote);
    auto it = g_lobbyClients.find(key);
    if (it == g_lobbyClients.end()) { SendError(remote, "Not in a room"); return; }
    LobbyClient& client = it->second;
    auto roomIt = g_lobbyRooms.find(client.room);
    if (roomIt == g_lobbyRooms.end()) { SendError(remote, "Room not found"); return; }
    LobbyRoom& room = roomIt->second;
    if (!client.host || room.hostKey != key) {
        SendError(remote, "Only the host can start the game");
        return;
    }
    if (room.members.size() < 2) {
        SendError(remote, "Wait for clients to join the room");
        return;
    }
    for (uint64_t memberKey : room.members) {
        auto memberIt = g_lobbyClients.find(memberKey);
        if (memberIt == g_lobbyClients.end()) continue;
        if (!memberIt->second.hasGame) {
            SendError(remote, "A player has not set the game path");
            return;
        }
    }
    room.started = true;

    LobbyStartPayload payload{};
    payload.mode = room.mode;
    for (uint64_t memberKey : room.members) {
        auto memberIt = g_lobbyClients.find(memberKey);
        if (memberIt == g_lobbyClients.end()) continue;
        SendMessage(memberIt->second.addr, MessageType::LobbyStart, 0, 0, 0, 0, 0,
                    reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    }
    printf("[lobby] %s started by host players=%zu mode=%u\n", room.code.c_str(),
           room.members.size(), static_cast<unsigned>(room.mode));
}

void RemoveSession(uint64_t key, const char* reason) {
    auto it = g_sessions.find(key);
    if (it == g_sessions.end()) return;
    Session& s = *it->second;
    std::string roomCode = s.room;
    printf("[%s] leave %s %s room=%s (%s)\n",
           IpToString(s.virtualIp).c_str(), s.name.c_str(), EndpointText(s.addr).c_str(),
           roomCode.c_str(), reason);
    auto roomIt = g_rooms.find(roomCode);
    if (roomIt != g_rooms.end()) {
        Room& room = roomIt->second;
        room.peers.erase(std::remove(room.peers.begin(), room.peers.end(), key), room.peers.end());
        g_sessions.erase(it);
        if (room.peers.empty()) {
            g_rooms.erase(roomIt);
            printf("[room] %s removed\n", roomCode.c_str());
        } else {
            BroadcastPeerList(room);
        }
    } else {
        g_sessions.erase(it);
    }
}

void HandleHello(const uint8_t* data, size_t size, const sockaddr_in& remote,
                 const RelayHeader& header) {
    std::string roomCode, name;
    uint64_t passwordHash = 0;
    if (!DecodeHello(data + kHeaderSize, header.length, roomCode, passwordHash, name)) {
        SendError(remote, "bad hello");
        return;
    }
    roomCode = NormalizeRoom(roomCode);
    name = SanitizeName(name);
    if (roomCode.empty()) { SendError(remote, "room required"); return; }

    uint64_t key = EndpointKey(remote);
    auto existingIt = g_sessions.find(key);
    if (existingIt != g_sessions.end()) {
        Session& prior = *existingIt->second;
        auto priorRoom = g_rooms.find(prior.room);
        if (prior.room == roomCode && prior.name == name &&
            priorRoom != g_rooms.end() && priorRoom->second.passwordHash == passwordHash) {
            prior.lastSeen = Clock::now();
            SendHelloAck(prior, priorRoom->second);
            PeerSnapshot snap = SnapshotRoom(priorRoom->second);
            uint8_t payload[kMaxDatagram];
            size_t len = EncodePeerList(payload, snap.names, snap.ips, snap.count);
            SendMessage(prior.addr, MessageType::PeerList, prior.token, 0, 0, 0, 0,
                        payload, static_cast<uint16_t>(len));
            printf("[re-ack] %s %s from %s room=%s\n", IpToString(prior.virtualIp).c_str(),
                   name.c_str(), EndpointText(remote).c_str(), roomCode.c_str());
            return;
        }
        RemoveSession(key, "rejoin");
    }

    auto roomIt = g_rooms.find(roomCode);
    if (roomIt != g_rooms.end()) {
        Room& room = roomIt->second;
        if (room.passwordHash != passwordHash) { SendError(remote, "bad room password"); return; }
        if (static_cast<int>(room.peers.size()) >= g_maxPeers) { SendError(remote, "room full"); return; }
    } else {
        Room room;
        room.code = roomCode;
        room.passwordHash = passwordHash;
        roomIt = g_rooms.emplace(roomCode, std::move(room)).first;
        printf("[room] %s created by %s\n", roomCode.c_str(), EndpointText(remote).c_str());
    }
    Room& room = roomIt->second;
    uint32_t virtualIp = AllocateVirtualIp(room);
    if (virtualIp == 0) { SendError(remote, "no free virtual ip"); return; }

    auto session = std::make_unique<Session>();
    session->key = key;
    session->addr = remote;
    session->token = NewToken();
    session->virtualIp = virtualIp;
    session->name = name;
    session->room = roomCode;
    session->lastSeen = Clock::now();
    Session* sessionPtr = session.get();
    g_sessions.emplace(key, std::move(session));
    room.peers.push_back(key);

    SendHelloAck(*sessionPtr, room);
    printf("[join] %s %s from %s room=%s players=%zu\n",
           IpToString(virtualIp).c_str(), name.c_str(), EndpointText(remote).c_str(),
           roomCode.c_str(), room.peers.size());
    BroadcastPeerList(room);
}

void HandleData(const uint8_t* data, const RelayHeader& header, const sockaddr_in& remote) {
    auto it = g_sessions.find(EndpointKey(remote));
    if (it == g_sessions.end()) return;
    Session& from = *it->second;
    if (from.token != header.token) return;
    from.lastSeen = Clock::now();
    if (header.dstVirt == 0 || header.length == 0) return;
    auto roomIt = g_rooms.find(from.room);
    if (roomIt == g_rooms.end()) return;
    for (uint64_t key : roomIt->second.peers) {
        auto targetIt = g_sessions.find(key);
        if (targetIt == g_sessions.end()) continue;
        Session& target = *targetIt->second;
        if (target.virtualIp != header.dstVirt || target.key == from.key) continue;
        SendMessage(target.addr, MessageType::Data, target.token, from.virtualIp, 0,
                    header.srcPort, header.dstPort,
                    data + kHeaderSize, header.length);
        return;
    }
}

void HandleBroadcast(const uint8_t* data, size_t size, const sockaddr_in& remote,
                     const RelayHeader& header) {
    auto it = g_sessions.find(EndpointKey(remote));
    if (it == g_sessions.end()) return;
    Session& from = *it->second;
    if (from.token != header.token) return;
    from.lastSeen = Clock::now();
    auto roomIt = g_rooms.find(from.room);
    if (roomIt == g_rooms.end()) return;
    uint16_t srcPort = header.srcPort ? header.srcPort : kDiscoveryPort;
    for (uint64_t key : roomIt->second.peers) {
        auto targetIt = g_sessions.find(key);
        if (targetIt == g_sessions.end()) continue;
        Session& target = *targetIt->second;
        if (target.key == from.key) continue;
        SendMessage(target.addr, MessageType::Broadcast, target.token, from.virtualIp, 0,
                    srcPort, kDiscoveryPort,
                    data + kHeaderSize, header.length);
    }
}

void HandleChat(const uint8_t* data, const RelayHeader& header, const sockaddr_in& remote) {
    auto it = g_sessions.find(EndpointKey(remote));
    if (it == g_sessions.end()) return;
    Session& from = *it->second;
    if (from.token != header.token) return;
    from.lastSeen = Clock::now();

    std::string text = SanitizeChat(
        std::string(reinterpret_cast<const char*>(data + kHeaderSize), header.length));
    if (text.empty()) return;

    auto roomIt = g_rooms.find(from.room);
    if (roomIt == g_rooms.end()) return;

    uint8_t payload[kMaxPayload];
    size_t length = EncodeChat(payload, from.name, text);
    for (uint64_t key : roomIt->second.peers) {
        auto targetIt = g_sessions.find(key);
        if (targetIt == g_sessions.end()) continue;
        Session& target = *targetIt->second;
        SendMessage(target.addr, MessageType::Chat, target.token, from.virtualIp, 0, 0, 0,
                    payload, static_cast<uint16_t>(length));
    }
    printf("[chat] %s room=%s bytes=%zu\n", from.name.c_str(), from.room.c_str(), text.size());
}

void HandlePlainPacket(uint8_t* data, size_t size, const sockaddr_in& remote) {
    RelayHeader header{};
    if (!DecodeHeader(data, size, header)) {
        printf("[drop] %s bad header\n", EndpointText(remote).c_str());
        return;
    }
    if (header.length > size - kHeaderSize) {
        printf("[drop] %s truncated packet type=%u len=%u received=%zu\n",
               EndpointText(remote).c_str(), static_cast<unsigned>(header.type),
               static_cast<unsigned>(header.length), size);
        return;
    }
    switch (header.type) {
        case MessageType::Hello: HandleHello(data, size, remote, header); break;
        case MessageType::Data: HandleData(data, header, remote); break;
        case MessageType::Broadcast: HandleBroadcast(data, size, remote, header); break;
        case MessageType::Chat: HandleChat(data, header, remote); break;
        case MessageType::Ping: {
            auto it = g_sessions.find(EndpointKey(remote));
            if (it != g_sessions.end() && it->second->token == header.token) {
                it->second->lastSeen = Clock::now();
                SendMessage(remote, MessageType::Pong, it->second->token, 0, 0, 0, 0, nullptr, 0);
            }
            break;
        }
        case MessageType::Bye: {
            auto it = g_sessions.find(EndpointKey(remote));
            if (it != g_sessions.end() && it->second->token == header.token)
                RemoveSession(it->first, "bye");
            break;
        }
        case MessageType::LobbyCreate: HandleLobbyCreate(data, remote, header); break;
        case MessageType::LobbyJoin: HandleLobbyJoin(data, remote, header); break;
        case MessageType::LobbyLeave: HandleLobbyLeave(remote); break;
        case MessageType::LobbyPing: HandleLobbyPing(data, remote, header); break;
        case MessageType::LobbyStart: HandleLobbyStart(data, size, remote, header); break;
        case MessageType::LobbyBrowse: HandleLobbyBrowse(remote); break;
        case MessageType::LobbyKick: HandleLobbyKick(data, size, remote, header); break;
        case MessageType::LobbyChat: HandleLobbyChat(data, size, remote, header); break;
        default: break;
    }
}

// Transport layer: every client/hook connection starts with an X25519
// handshake and then carries only authenticated ciphertext.
void HandlePacket(uint8_t* data, size_t size, const sockaddr_in& remote) {
    if (!ngs::IsSecureFrame(data, size)) {
        printf("[drop] %s unencrypted datagram\n", EndpointText(remote).c_str());
        return;
    }
    if (data[4] != ngs::kVersion) return;

    const uint64_t key = EndpointKey(remote);
    const uint8_t frameType = data[5];

    if (frameType == ngs::FrameInit) {
        ngs::Channel& channel = g_channels[key];
        uint8_t reply[ngs::kMaxFrame];
        size_t replyLength = 0;
        if (channel.HandleInit(data, size, g_staticPriv, g_staticPub, reply, replyLength,
                               NowMs())) {
            SendRaw(remote, reply, replyLength);
            printf("[secure] handshake %s\n", EndpointText(remote).c_str());
        }
        return;
    }

    auto it = g_channels.find(key);
    if (it == g_channels.end() || !it->second.ready) return;
    if (frameType != ngs::FrameData) return;

    uint8_t plain[ngs::kMaxPlain];
    size_t plainLength = 0;
    if (!it->second.Open(data, size, plain, plainLength)) return;
    it->second.lastSeenMs = NowMs();
    HandlePlainPacket(plain, plainLength, remote);
}

void ExpireSessions() {
    auto cutoff = Clock::now() - std::chrono::seconds(60);
    std::vector<uint64_t> expired;
    for (auto& entry : g_sessions) {
        if (entry.second->lastSeen < cutoff) expired.push_back(entry.first);
    }
    for (uint64_t key : expired) RemoveSession(key, "timeout");

    // Drop encrypted transport state that has been idle for two minutes.
    uint64_t nowMs = NowMs();
    for (auto it = g_channels.begin(); it != g_channels.end();) {
        if (it->second.lastSeenMs == 0 || nowMs - it->second.lastSeenMs > 120000)
            it = g_channels.erase(it);
        else
            ++it;
    }
}

void ExpireLobbies() {
    auto cutoff = Clock::now() - std::chrono::seconds(30);
    std::vector<uint64_t> expired;
    for (auto& entry : g_lobbyClients) {
        if (entry.second.lastSeen < cutoff) expired.push_back(entry.first);
    }
    for (uint64_t key : expired) RemoveLobbyClient(key, "timeout");
}

#ifdef _WIN32
BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}
#else
void SignalHandler(int) { g_stop = true; }
#endif

bool SetSocketBuffers() {
    int size = 4 * 1024 * 1024;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    int tos = 0xB8; // DSCP EF
    setsockopt(g_sock, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));
#ifdef _WIN32
    BOOL noReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), nullptr, 0, &bytes, nullptr, nullptr);
#else
    int priority = 6;
    setsockopt(g_sock, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority));
    int busyPoll = 50;
#  ifdef SO_BUSY_POLL
    setsockopt(g_sock, SOL_SOCKET, SO_BUSY_POLL, &busyPoll, sizeof(busyPoll));
#  endif
#endif
    return true;
}

bool OpenSocket(const Options& options) {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return false;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { fprintf(stderr, "socket failed: %d\n", WSAGetLastError()); return false; }
    u_long nonBlocking = 1;
    ioctlsocket(g_sock, FIONBIO, &nonBlocking);
#else
    g_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (g_sock < 0) { perror("socket"); return false; }
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(options.port));
    addr.sin_addr.s_addr = IpFromString(options.bindAddress.c_str());
    if (bind(g_sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
        fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
#else
        perror("bind");
#endif
        return false;
    }
    SetSocketBuffers();
    return true;
}

// Loads (or creates) the long-term server key used for handshake
// authentication.  The key is a 32-byte X25519 private key stored as a raw
// file; clients pin the derived public key on first connect.
bool LoadStaticKey(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file) {
        size_t got = fread(g_staticPriv, 1, sizeof(g_staticPriv), file);
        fclose(file);
        if (got != sizeof(g_staticPriv)) {
            fprintf(stderr, "[secure] key file %s must contain 32 bytes\n", path.c_str());
            return false;
        }
    } else {
        ngc::RandomBytes(g_staticPriv, sizeof(g_staticPriv));
        FILE* out = fopen(path.c_str(), "wb");
        if (!out) {
            fprintf(stderr, "[secure] cannot create the key file %s\n", path.c_str());
            return false;
        }
        bool ok = fwrite(g_staticPriv, 1, sizeof(g_staticPriv), out) == sizeof(g_staticPriv);
        fclose(out);
        if (!ok) {
            fprintf(stderr, "[secure] cannot write the key file %s\n", path.c_str());
            return false;
        }
#ifndef _WIN32
        chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
        printf("[secure] generated a new server key: %s\n", path.c_str());
    }
    ngc::X25519Base(g_staticPub, g_staticPriv);
    return true;
}

int Run(const Options& options) {
    if (!LoadStaticKey(options.keyFile)) return 1;
    if (!OpenSocket(options)) return 1;

#ifndef _WIN32
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return 1; }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = g_sock;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_sock, &ev) != 0) { perror("epoll_ctl"); return 1; }
#endif

    uint8_t buffer[ngs::kMaxFrame];
    auto lastExpiry = Clock::now();
    printf("[start] udp/%d subnet=%s mask=%s max-peers=%d encryption=x25519+xsalsa20-poly1305\n",
           options.port, IpToString(g_subnet).c_str(), IpToString(g_mask).c_str(), g_maxPeers);
    printf("[secure] server key %s fingerprint %s\n", options.keyFile.c_str(),
           ngp::Fingerprint(g_staticPub).c_str());
    printf("[secure] clients pin this key on first connect; delete the key file to rotate it\n");
    fflush(stdout);

    while (!g_stop) {
#ifdef _WIN32
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(g_sock, &readSet);
        timeval timeout{1, 0};
        int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0) break;
        if (ready == 0) {
            if (Clock::now() - lastExpiry >= std::chrono::seconds(5)) {
                lastExpiry = Clock::now();
                ExpireSessions();
                ExpireLobbies();
            }
            continue;
        }
        for (;;) {
            sockaddr_in remote{};
            int remoteLen = sizeof(remote);
            int received = recvfrom(g_sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr*>(&remote), &remoteLen);
            if (received == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) break;
                if (error == WSAECONNRESET) continue;
                break;
            }
            HandlePacket(buffer, static_cast<size_t>(received), remote);
        }
#else
        epoll_event events[8];
        int count = epoll_wait(epfd, events, 8, 1000);
        if (count < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < count; ++i) {
            for (;;) {
                sockaddr_in remote{};
                socklen_t remoteLen = sizeof(remote);
                ssize_t received = recvfrom(g_sock, buffer, sizeof(buffer), 0,
                                            reinterpret_cast<sockaddr*>(&remote), &remoteLen);
                if (received < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    if (errno == ECONNREFUSED) continue;
                    perror("recvfrom");
                    break;
                }
                HandlePacket(buffer, static_cast<size_t>(received), remote);
            }
        }
#endif
        if (Clock::now() - lastExpiry >= std::chrono::seconds(5)) {
            lastExpiry = Clock::now();
            ExpireSessions();
            ExpireLobbies();
        }
    }

#ifdef _WIN32
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    WSACleanup();
#else
    if (g_sock >= 0) close(g_sock);
    close(epfd);
#endif
    printf("[stop]\n");
    return 0;
}

void PrintHelp() {
    printf(
        "Net Goal 11.Server " NETGOAL11_VERSION_A " (native C++)\n"
        "  --port <udp-port>       default %d\n"
        "  --subnet <ipv4>         default 10.213.77.0\n"
        "  --mask <ipv4>           default 255.255.255.0\n"
        "  --max-peers <2..16>     default 16\n"
        "  --bind <ipv4>           default 0.0.0.0\n"
        "  --key-file <path>       default net-goal-11-server.key\n"
        "  --help\n",
        kDefaultPort);
}

bool ParseOptions(int argc, char** argv, Options& options) {
    std::string subnetText = "10.213.77.0";
    std::string maskText = "255.255.255.0";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", name); return nullptr; }
            return argv[++i];
        };
        if (arg == "--port") { const char* v = next("--port"); if (!v) return false; options.port = atoi(v); }
        else if (arg == "--subnet") { const char* v = next("--subnet"); if (!v) return false; subnetText = v; }
        else if (arg == "--mask") { const char* v = next("--mask"); if (!v) return false; maskText = v; }
        else if (arg == "--max-peers") { const char* v = next("--max-peers"); if (!v) return false; options.maxPeers = atoi(v); }
        else if (arg == "--bind") { const char* v = next("--bind"); if (!v) return false; options.bindAddress = v; }
        else if (arg == "--key-file") { const char* v = next("--key-file"); if (!v) return false; options.keyFile = v; }
        else if (arg == "--help" || arg == "-h") { PrintHelp(); exit(0); }
        else { fprintf(stderr, "unknown argument: %s\n", arg.c_str()); return false; }
    }
    if (options.port < 1 || options.port > 65535) { fprintf(stderr, "invalid port\n"); return false; }
    if (options.maxPeers < 2 || options.maxPeers > static_cast<int>(kMaxPeers)) {
        fprintf(stderr, "invalid max-peers\n"); return false;
    }
    options.mask = IpFromString(maskText.c_str());
    options.subnet = IpFromString(subnetText.c_str());
    if (options.mask == 0 || options.subnet == 0) { fprintf(stderr, "invalid subnet/mask\n"); return false; }
    int prefix = PrefixFromMask(options.mask);
    if (prefix < 8 || prefix > 30) { fprintf(stderr, "mask must be /8../30\n"); return false; }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options options;
    if (!ParseOptions(argc, argv, options)) return 1;
    g_mask = options.mask;
    g_subnet = options.subnet & options.mask;
    g_network = g_subnet;
    g_maxPeers = options.maxPeers;
#ifdef _WIN32
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
#else
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN);
#endif
    return Run(options);
}
