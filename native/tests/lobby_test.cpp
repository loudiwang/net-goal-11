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

#include "secure_test_peer.h"

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++g_fail; } } while (0)

using Peer = SecureTestPeer;

static bool OpenPeer(Peer& peer, int port) {
    return peer.Open("127.0.0.1", port);
}

static bool SendLobby(Peer& peer, fr::MessageType type, uint8_t mode,
                      bool hasGame, uint64_t hash, const char* room,
                      const char* name) {
    uint8_t payload[fr::kMaxPayload] = {};
    size_t len = fr::EncodeLobbyRequest(payload, mode, hasGame, hash, room, name);
    uint8_t packet[fr::kHeaderSize + fr::kMaxPayload] = {};
    if (!fr::EncodeHeader(packet, type, 0, 0, 0, 0, 0, (uint16_t)len)) return false;
    memcpy(packet + fr::kHeaderSize, payload, len);
    return peer.Send(packet, fr::kHeaderSize + len);
}

static bool SendStart(Peer& peer) {
    fr::LobbyStartPayload payload{};
    payload.mode = 1;
    uint8_t packet[fr::kHeaderSize + sizeof(payload)] = {};
    if (!fr::EncodeHeader(packet, fr::LobbyStart, 0, 0, 0, 0, 0, sizeof(payload))) return false;
    memcpy(packet + fr::kHeaderSize, &payload, sizeof(payload));
    return peer.Send(packet, sizeof(packet));
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

static State Receive(Peer& peer) {
    for (;;) {
        State state;
        uint8_t buffer[2048];
        int received = peer.Receive(buffer, sizeof(buffer));
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
        // Room chat notifications are not part of the sequencing checked here.
        if (state.type == fr::LobbyChatMessage) continue;
        return state;
    }
}

// Reads messages until one of the wanted type arrives (skipping chat/state
// notifications that are pushed asynchronously by the server).
static State ReceiveUntil(Peer& peer, uint8_t wanted) {
    for (int i = 0; i < 8; ++i) {
        State state = Receive(peer);
        if (state.type == 0) return state;
        if (state.type == wanted) return state;
    }
    return State{};
}

static void Drain(Peer& peer) {
    uint8_t buffer[2048];
    while (peer.Receive(buffer, sizeof(buffer)) >= 0) {
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
    State startHost = ReceiveUntil(host, fr::LobbyStart);
    State startGuest = ReceiveUntil(guest, fr::LobbyStart);
    CHECK(startHost.type == fr::LobbyStart);
    CHECK(startGuest.type == fr::LobbyStart);

    // Starting again is idempotent (allows retry if a UDP packet was lost).
    CHECK(SendStart(host));
    CHECK(Receive(host).type == fr::LobbyStart);
    CHECK(Receive(guest).type == fr::LobbyStart);

    // A new room with a guest that has no game path must be rejected at start.
    CHECK(SendLobby(host, fr::LobbyCreate, 1, true, 0x3333, "TESTLOBBY2", "Host2"));
    State s2 = ReceiveUntil(host, fr::LobbyState);
    CHECK(s2.type == fr::LobbyState);
    Drain(guest);  // "host left the old room"
    CHECK(SendLobby(guest, fr::LobbyJoin, 0, false, 0x3333, "TESTLOBBY2", "Guest2"));
    CHECK(Receive(guest).type == fr::LobbyState);
    CHECK(Receive(host).type == fr::LobbyState);
    CHECK(SendStart(host));
    State blocked = ReceiveUntil(host, fr::Error);
    CHECK(blocked.type == fr::Error);

    printf(g_fail ? "lobby_test: %d failure(s)\n" : "lobby_test: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
