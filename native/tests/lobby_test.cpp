// Small local test for the lobby messages.  Not part of the shipped client.
#include "../common/protocol.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++g_fail; } } while (0)

struct Peer {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in server{};
};

static bool OpenPeer(Peer& peer, int port) {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    peer.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (peer.s == INVALID_SOCKET) return false;
#ifdef _WIN32
    DWORD timeout = 1500;
#else
    timeval timeout{1, 500000};
#endif
    setsockopt(peer.s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    peer.server.sin_family = AF_INET;
    peer.server.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &peer.server.sin_addr);
    return true;
}

static bool SendLobby(const Peer& peer, fr::MessageType type, uint8_t mode,
                      bool hasGame, uint64_t hash, const char* room,
                      const char* name) {
    uint8_t payload[fr::kMaxPayload] = {};
    size_t len = fr::EncodeLobbyRequest(payload, mode, hasGame, hash, room, name);
    uint8_t packet[fr::kHeaderSize + fr::kMaxPayload] = {};
    if (!fr::EncodeHeader(packet, type, 0, 0, 0, 0, 0, (uint16_t)len)) return false;
    memcpy(packet + fr::kHeaderSize, payload, len);
    int sent = sendto(peer.s, (const char*)packet, (int)(fr::kHeaderSize + len), 0,
                      (const sockaddr*)&peer.server, sizeof(peer.server));
    return sent == (int)(fr::kHeaderSize + len);
}

static bool SendStart(const Peer& peer) {
    fr::LobbyStartPayload payload{};
    payload.mode = 1;
    uint8_t packet[fr::kHeaderSize + sizeof(payload)] = {};
    if (!fr::EncodeHeader(packet, fr::LobbyStart, 0, 0, 0, 0, 0, sizeof(payload))) return false;
    memcpy(packet + fr::kHeaderSize, &payload, sizeof(payload));
    int sent = sendto(peer.s, (const char*)packet, (int)sizeof(packet), 0,
                      (const sockaddr*)&peer.server, sizeof(peer.server));
    return sent == (int)sizeof(packet);
}

struct State {
    uint8_t type = 0;
    uint8_t mode = 0;
    bool selfHost = false;
    std::string room;
    int count = 0;
    std::string names[fr::kMaxPeers];
    uint8_t flags[fr::kMaxPeers] = {};
};

static State Receive(const Peer& peer) {
    State state;
    uint8_t buffer[2048];
    sockaddr_in from{};
    int fromLen = sizeof(from);
    int received = recvfrom(peer.s, (char*)buffer, sizeof(buffer), 0,
                            (sockaddr*)&from, &fromLen);
    if (received < (int)fr::kHeaderSize) return state;
    fr::RelayHeader header{};
    if (!fr::DecodeHeader(buffer, (size_t)received, header)) return state;
    state.type = header.type;
    if (header.type == fr::LobbyState) {
        CHECK(fr::DecodeLobbyState(buffer + fr::kHeaderSize, header.length, state.mode,
                                   state.selfHost, state.room, state.names,
                                   state.flags, state.count));
    } else {
        state.count = header.length;
    }
    return state;
}

static void Drain(const Peer& peer) {
    uint8_t buffer[2048];
    for (;;) {
        int received = recvfrom(peer.s, (char*)buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (received == SOCKET_ERROR) break;
    }
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 45999;
    Peer host, guest, other;
    CHECK(OpenPeer(host, port));
    CHECK(OpenPeer(guest, port));
    CHECK(OpenPeer(other, port));

    CHECK(SendLobby(host, fr::LobbyCreate, 2, true, 0x1111, "TESTLOBBY", "Host"));
    State s = Receive(host);
    CHECK(s.type == fr::LobbyState);
    CHECK(s.mode == 2);
    CHECK(s.selfHost);
    CHECK(s.count == 1);
    CHECK(s.room == "TESTLOBBY");

    CHECK(SendLobby(guest, fr::LobbyJoin, 0, true, 0x1111, "TESTLOBBY", "Guest"));
    State gs = Receive(guest);
    CHECK(gs.type == fr::LobbyState);
    CHECK(!gs.selfHost);
    CHECK(gs.count == 2);
    State hs = Receive(host);
    CHECK(hs.type == fr::LobbyState);
    CHECK(hs.selfHost);
    CHECK(hs.count == 2);

    // Wrong password must be rejected.
    CHECK(SendLobby(other, fr::LobbyJoin, 0, true, 0x2222, "TESTLOBBY", "Other"));
    State bad = Receive(other);
    CHECK(bad.type == fr::Error);

    // Host start must reach both GUI clients.
    CHECK(SendStart(host));
    State startHost = Receive(host);
    State startGuest = Receive(guest);
    CHECK(startHost.type == fr::LobbyStart);
    CHECK(startGuest.type == fr::LobbyStart);

    // Starting again is idempotent (allows retry if a UDP packet was lost).
    CHECK(SendStart(host));
    CHECK(Receive(host).type == fr::LobbyStart);
    CHECK(Receive(guest).type == fr::LobbyStart);

    // A new room with a guest that has no game path must be rejected at start.
    CHECK(SendLobby(host, fr::LobbyCreate, 1, true, 0x3333, "TESTLOBBY2", "Host2"));
    State s2 = Receive(host);
    CHECK(s2.type == fr::LobbyState);
    Drain(guest);  // "host left the old room"
    CHECK(SendLobby(guest, fr::LobbyJoin, 0, false, 0x3333, "TESTLOBBY2", "Guest2"));
    CHECK(Receive(guest).type == fr::LobbyState);
    CHECK(Receive(host).type == fr::LobbyState);
    CHECK(SendStart(host));
    State blocked = Receive(host);
    CHECK(blocked.type == fr::Error);

    printf(g_fail ? "lobby_test: %d failure(s)\n" : "lobby_test: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
