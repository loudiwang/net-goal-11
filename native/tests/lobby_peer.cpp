// Test helper: creates or joins a lobby room and keeps it alive with pings.
// Usage: lobby_peer.exe <port> <create|join|browse> <room> <mode> <name> [seconds]
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../common/protocol.h"
#include "secure_test_peer.h"

#pragma comment(lib, "ws2_32.lib")
using namespace fr;

int main(int argc, char** argv) {
    if (argc < 6) {
        printf("usage: lobby_peer.exe <port> <create|join|browse> <room> <mode> <name> [seconds]\n");
        return 1;
    }
    int port = atoi(argv[1]);
    bool create = strcmp(argv[2], "create") == 0;
    bool browse = strcmp(argv[2], "browse") == 0;
    const char* room = argv[3];
    uint8_t mode = (uint8_t)atoi(argv[4]);
    const char* name = argv[5];
    int seconds = argc > 6 ? atoi(argv[6]) : 300;

    SecureTestPeer peer;
    if (!peer.Open("127.0.0.1", port)) {
        printf("[peer] encrypted handshake failed\n");
        return 1;
    }
    printf("[peer] encrypted channel established\n");
    fflush(stdout);

    uint8_t buffer[kMaxDatagram];

    if (browse) {
        EncodeHeader(buffer, MessageType::LobbyBrowse, 0, 0, 0, 0, 0, 0);
        peer.Send(buffer, kHeaderSize);
        uint8_t reply[2048];
        int received = peer.Receive(reply, sizeof(reply));
        if (received >= (int)kHeaderSize) {
            RelayHeader header{};
            if (DecodeHeader(reply, (size_t)received, header) &&
                header.type == MessageType::LobbyList &&
                header.length >= sizeof(LobbyListPayload)) {
                const auto* list = reinterpret_cast<const LobbyListPayload*>(reply + kHeaderSize);
                for (int i = 0; i < list->count && i < kMaxLobbyListRooms; ++i) {
                    const auto& entry = list->rooms[i];
                    printf("room=%.*s mode=%u members=%u/%u host=%.*s flags=%u\n",
                           (int)kRoomLength, entry.code, (unsigned)entry.mode,
                           (unsigned)entry.members, (unsigned)entry.capacity,
                           (int)kNameLength, entry.host, (unsigned)entry.flags);
                }
            }
        }
        peer.Close();
        return 0;
    }

    LobbyRequestPayload request{};
    EncodeLobbyRequest((uint8_t*)&request, mode, true, 0, room, name);
    EncodeHeader(buffer, create ? MessageType::LobbyCreate : MessageType::LobbyJoin,
                 0, 0, 0, 0, 0, sizeof(request));
    memcpy(buffer + kHeaderSize, &request, sizeof(request));
    peer.Send(buffer, kHeaderSize + sizeof(request));
    printf("[peer] %s room=%s mode=%u name=%s\n", create ? "create" : "join", room,
           (unsigned)mode, name);
    fflush(stdout);

    for (int elapsed = 0; elapsed < seconds; elapsed += 5) {
        uint8_t reply[2048];
        int received = peer.Receive(reply, sizeof(reply));
        if (received >= (int)kHeaderSize) {
            RelayHeader header{};
            if (DecodeHeader(reply, (size_t)received, header)) {
                if (header.type == MessageType::LobbyState) {
                    printf("[peer] state members=%u selfHost=%u\n",
                           (unsigned)(uint8_t)reply[kHeaderSize + 1],
                           (unsigned)(uint8_t)reply[kHeaderSize + 2]);
                    fflush(stdout);
                } else if (header.type == MessageType::LobbyStart) {
                    printf("[peer] LobbyStart received\n");
                    fflush(stdout);
                } else if (header.type == MessageType::Error) {
                    std::string text((const char*)reply + kHeaderSize, header.length);
                    printf("[peer] error: %s\n", text.c_str());
                    fflush(stdout);
                    break;
                }
            }
        }
        EncodeHeader(buffer, MessageType::LobbyPing, 0, 0, 0, 0, 0, sizeof(request));
        memcpy(buffer + kHeaderSize, &request, sizeof(request));
        peer.Send(buffer, kHeaderSize + sizeof(request));
        Sleep(5000);
    }
    peer.Close();
    return 0;
}
