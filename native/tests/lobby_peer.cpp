// Test helper: creates or joins a lobby room and keeps it alive with pings.
// Usage: lobby_peer.exe <port> <create|join> <room> <mode> <name> [seconds]
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

int main(int argc, char** argv) {
    if (argc < 6) {
        printf("usage: lobby_peer.exe <port> <create|join> <room> <mode> <name> [seconds]\n");
        return 1;
    }
    int port = atoi(argv[1]);
    bool create = strcmp(argv[2], "create") == 0;
    bool browse = strcmp(argv[2], "browse") == 0;
    const char* room = argv[3];
    uint8_t mode = (uint8_t)atoi(argv[4]);
    const char* name = argv[5];
    int seconds = argc > 6 ? atoi(argv[6]) : 300;

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);
    uint8_t buffer[kMaxDatagram];

    if (browse) {
        DWORD timeout = 1500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        EncodeHeader(buffer, MessageType::LobbyBrowse, 0, 0, 0, 0, 0, 0);
        sendto(s, (const char*)buffer, kHeaderSize, 0, (const sockaddr*)&server,
               sizeof(server));
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int received = recvfrom(s, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from,
                                &fromLen);
        if (received >= (int)kHeaderSize) {
            RelayHeader header{};
            if (DecodeHeader(buffer, (size_t)received, header) &&
                header.type == MessageType::LobbyList &&
                header.length >= sizeof(LobbyListPayload)) {
                const auto* list = reinterpret_cast<const LobbyListPayload*>(buffer + kHeaderSize);
                for (int i = 0; i < list->count && i < kMaxLobbyListRooms; ++i) {
                    const auto& entry = list->rooms[i];
                    printf("room=%.*s mode=%u members=%u/%u host=%.*s flags=%u\n",
                           (int)kRoomLength, entry.code, (unsigned)entry.mode,
                           (unsigned)entry.members, (unsigned)entry.capacity,
                           (int)kNameLength, entry.host, (unsigned)entry.flags);
                }
            }
        }
        closesocket(s);
        WSACleanup();
        return 0;
    }

    LobbyRequestPayload request{};
    EncodeLobbyRequest((uint8_t*)&request, mode, true, 0, room, name);
    EncodeHeader(buffer, create ? MessageType::LobbyCreate : MessageType::LobbyJoin,
                 0, 0, 0, 0, 0, sizeof(request));
    memcpy(buffer + kHeaderSize, &request, sizeof(request));
    sendto(s, (const char*)buffer, kHeaderSize + sizeof(request), 0,
           (const sockaddr*)&server, sizeof(server));
    printf("[peer] %s room=%s mode=%u name=%s\n", create ? "create" : "join", room,
           (unsigned)mode, name);
    fflush(stdout);

    for (int elapsed = 0; elapsed < seconds; elapsed += 5) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        DWORD timeout = 900;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        int received = recvfrom(s, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &fromLen);
        if (received >= (int)kHeaderSize) {
            RelayHeader header{};
            if (DecodeHeader(buffer, (size_t)received, header)) {
                if (header.type == MessageType::LobbyState) {
                    printf("[peer] state members=%u selfHost=%u\n",
                           (unsigned)(uint8_t)buffer[kHeaderSize + 1],
                           (unsigned)(uint8_t)buffer[kHeaderSize + 2]);
                    fflush(stdout);
                } else if (header.type == MessageType::LobbyStart) {
                    printf("[peer] LobbyStart received\n");
                    fflush(stdout);
                } else if (header.type == MessageType::Error) {
                    std::string text((const char*)buffer + kHeaderSize, header.length);
                    printf("[peer] error: %s\n", text.c_str());
                    fflush(stdout);
                    break;
                }
            }
        }
        EncodeHeader(buffer, MessageType::LobbyPing, 0, 0, 0, 0, 0, sizeof(request));
        memcpy(buffer + kHeaderSize, &request, sizeof(request));
        sendto(s, (const char*)buffer, kHeaderSize + sizeof(request), 0,
               (const sockaddr*)&server, sizeof(server));
        Sleep(5000);
    }
    closesocket(s);
    WSACleanup();
    return 0;
}
