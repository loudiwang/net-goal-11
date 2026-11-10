// Local protocol test for the room browser / kick / start flow.
// Usage: lobby_list_test.exe [port]
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../common/protocol.h"

#pragma comment(lib, "ws2_32.lib")

using namespace fr;

static sockaddr_in g_server{};
static int g_failures = 0;

static SOCKET OpenPeer(uint16_t& port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(s, (sockaddr*)&addr, sizeof(addr));
    int len = sizeof(addr);
    getsockname(s, (sockaddr*)&addr, &len);
    port = ntohs(addr.sin_port);
    DWORD timeout = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    return s;
}

static bool Send(SOCKET s, MessageType type, const void* payload, uint16_t length) {
    uint8_t buffer[kMaxDatagram];
    if (!EncodeHeader(buffer, type, 0, 0, 0, 0, 0, length)) return false;
    if (length) memcpy(buffer + kHeaderSize, payload, length);
    return sendto(s, (const char*)buffer, kHeaderSize + length, 0,
                  (const sockaddr*)&g_server, sizeof(g_server)) == (int)(kHeaderSize + length);
}

static bool Receive(SOCKET s, MessageType& type, uint8_t* payload, size_t& length) {
    uint8_t buffer[kMaxDatagram];
    sockaddr_in from{};
    int fromLen = sizeof(from);
    int received = recvfrom(s, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &fromLen);
    if (received < (int)kHeaderSize) return false;
    RelayHeader header{};
    if (!DecodeHeader(buffer, (size_t)received, header)) return false;
    type = (MessageType)header.type;
    length = header.length;
    if (length) memcpy(payload, buffer + kHeaderSize, length);
    return true;
}

static bool WaitFor(SOCKET s, MessageType wanted, uint8_t* payload, size_t& length) {
    for (int i = 0; i < 8; ++i) {
        MessageType type;
        if (!Receive(s, type, payload, length)) return false;
        if (type == wanted) return true;
    }
    return false;
}

static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) ++g_failures;
}

static void SendRequest(SOCKET s, MessageType type, uint8_t mode, const char* room,
                        const char* name, const char* password) {
    fr::LobbyRequestPayload request{};
    EncodeLobbyRequest((uint8_t*)&request, mode, true, PasswordHash(password),
                       room, name);
    Send(s, type, &request, sizeof(request));
}

static bool Browse(SOCKET s, LobbyListPayload& out) {
    if (!Send(s, MessageType::LobbyBrowse, nullptr, 0)) return false;
    uint8_t payload[1400];
    size_t length = 0;
    if (!WaitFor(s, MessageType::LobbyList, payload, length)) return false;
    if (length < sizeof(LobbyListPayload)) return false;
    memcpy(&out, payload, sizeof(out));
    return true;
}

static const LobbyListEntry* FindRoom(const LobbyListPayload& list, const char* code) {
    for (int i = 0; i < list.count && i < kMaxLobbyListRooms; ++i) {
        if (strncmp(list.rooms[i].code, code, kRoomLength) == 0) return &list.rooms[i];
    }
    return nullptr;
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 45888;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_server.sin_family = AF_INET;
    g_server.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &g_server.sin_addr);

    uint16_t hostPort = 0, guestPort = 0;
    SOCKET host = OpenPeer(hostPort);
    SOCKET guest = OpenPeer(guestPort);
    uint8_t payload[1400];
    size_t length = 0;

    SendRequest(host, MessageType::LobbyCreate, 2, "BROWSE1", "HostA", "");
    Check(WaitFor(host, MessageType::LobbyState, payload, length), "host receives LobbyState after create");

    LobbyListPayload list{};
    Check(Browse(guest, list), "guest browses room list");
    const LobbyListEntry* entry = FindRoom(list, "BROWSE1");
    Check(entry != nullptr, "created room appears in list");
    if (entry) {
        Check(entry->mode == 2 && entry->members == 1 && entry->capacity == 4,
              "room list shows mode 2, 1/4 members");
        Check(strncmp(entry->host, "HostA", kNameLength) == 0, "room list shows host name");
    }

    // 5v5 rooms must advertise a capacity of 10 players.
    uint16_t fivePort = 0;
    SOCKET five = OpenPeer(fivePort);
    SendRequest(five, MessageType::LobbyCreate, 5, "FIVE55", "HostFive", "");
    Check(WaitFor(five, MessageType::LobbyState, payload, length), "5v5 room created");
    Browse(guest, list);
    entry = FindRoom(list, "FIVE55");
    Check(entry && entry->mode == 5 && entry->capacity == 10,
          "5v5 room list shows capacity 10");
    Send(five, MessageType::LobbyLeave, nullptr, 0);
    closesocket(five);

    SendRequest(guest, MessageType::LobbyJoin, 0, "BROWSE1", "GuestB", "");
    Check(WaitFor(guest, MessageType::LobbyState, payload, length), "guest receives LobbyState after join");
    Check(WaitFor(host, MessageType::LobbyState, payload, length), "host sees guest join");
    {
        uint8_t mode = 0; bool selfHost = false; std::string room;
        std::string names[kMaxPeers]; uint8_t flags[kMaxPeers] = {}; int count = 0;
        DecodeLobbyState(payload, length, mode, selfHost, room, names, flags, count);
        Check(count == 2 && selfHost, "host state has 2 members and host flag is set");
    }

    Check(WaitFor(host, MessageType::LobbyChatMessage, payload, length),
          "host receives join welcome message");
    if (length >= sizeof(LobbyChatMessagePayload)) {
        const auto* welcome = reinterpret_cast<const LobbyChatMessagePayload*>(payload);
        std::string text(welcome->text, strnlen(welcome->text, sizeof(welcome->text)));
        Check(welcome->system != 0 && text.find("欢迎") != std::string::npos &&
                  text.find("BROWSE1") != std::string::npos,
              "join welcome text contains player and room");
    }

    LobbyChatPayload chat{};
    strncpy(chat.text, "hello room", sizeof(chat.text) - 1);
    Send(guest, MessageType::LobbyChat, &chat, sizeof(chat));
    Check(WaitFor(host, MessageType::LobbyChatMessage, payload, length),
          "host receives room chat message");
    if (length >= sizeof(LobbyChatMessagePayload)) {
        const auto* message = reinterpret_cast<const LobbyChatMessagePayload*>(payload);
        std::string text(message->text, strnlen(message->text, sizeof(message->text)));
        Check(message->system == 0 && text == "hello room", "room chat text is forwarded");
    }

    Browse(guest, list);
    entry = FindRoom(list, "BROWSE1");
    Check(entry && entry->members == 2, "room list shows 2/4 after join");

    // Kick slot 1 (the guest).
    LobbyKickPayload kick{};
    kick.index = 1;
    Send(host, MessageType::LobbyKick, &kick, sizeof(kick));
    Check(WaitFor(guest, MessageType::Error, payload, length), "kicked guest receives Error");
    if (length) {
        std::string text((const char*)payload, length);
        Check(text.find("移出房间") != std::string::npos, "kick error text mentions removal");
    }
    Check(WaitFor(host, MessageType::LobbyState, payload, length), "host gets state after kick");
    Browse(guest, list);
    entry = FindRoom(list, "BROWSE1");
    Check(entry && entry->members == 1, "room list shows 1/4 after kick");

    // Start with one member must fail.
    Send(host, MessageType::LobbyStart, nullptr, 0);
    Check(WaitFor(host, MessageType::Error, payload, length), "start with one member is rejected");

    // Rejoin and start.
    SendRequest(guest, MessageType::LobbyJoin, 0, "BROWSE1", "GuestB", "");
    Check(WaitFor(guest, MessageType::LobbyState, payload, length), "guest rejoins");
    Send(host, MessageType::LobbyStart, nullptr, 0);
    Check(WaitFor(host, MessageType::LobbyStart, payload, length), "host receives LobbyStart");
    Check(WaitFor(guest, MessageType::LobbyStart, payload, length), "guest receives LobbyStart");

    Send(host, MessageType::LobbyLeave, nullptr, 0);
    closesocket(host);
    closesocket(guest);
    WSACleanup();
    printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
