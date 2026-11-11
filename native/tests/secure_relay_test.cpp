// End-to-end test for the encrypted relay path.
//
// Starts two relay clients against a running server, performs the encrypted
// handshake, joins both into one room and forwards a data packet between them.
//
// Usage: secure_relay_test.exe [port]
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/protocol.h"
#include "../common/secure.h"

#pragma comment(lib, "ws2_32.lib")

using namespace fr;

static int g_failures = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++g_failures; } } while (0)

struct Peer {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in server{};
    ngs::Channel channel;
    uint8_t serverKey[32] = {};
    uint64_t token = 0;
    uint32_t virtualIp = 0;
};

static bool Handshake(Peer& peer) {
    peer.channel.BeginClientHandshake(GetTickCount64());
    uint8_t init[ngs::kMaxFrame];
    size_t initLength = 0;
    peer.channel.BuildInit(init, initLength);

    ULONGLONG deadline = GetTickCount64() + 3000;
    ULONGLONG nextSend = 0;
    while (GetTickCount64() < deadline) {
        if (GetTickCount64() >= nextSend) {
            sendto(peer.socket, (const char*)init, (int)initLength, 0,
                   (const sockaddr*)&peer.server, sizeof(peer.server));
            nextSend = GetTickCount64() + 300;
        }
        uint8_t buffer[ngs::kMaxFrame];
        int received = recv(peer.socket, (char*)buffer, sizeof(buffer), 0);
        if (received <= 0) continue;
        if (!ngs::IsSecureFrame(buffer, (size_t)received)) continue;
        if (buffer[5] != ngs::FrameReply) continue;
        if (!ngs::Channel::PeekReplyStatic(buffer, (size_t)received, peer.serverKey)) continue;
        if (peer.channel.HandleReply(buffer, (size_t)received, nullptr, GetTickCount64()))
            return true;
    }
    return false;
}

static bool SendPlain(Peer& peer, MessageType type, uint64_t token, uint32_t srcVirt,
                      uint32_t dstVirt, uint16_t srcPort, uint16_t dstPort,
                      const uint8_t* payload, uint16_t length) {
    uint8_t plain[fr::kMaxDatagram];
    if (!EncodeHeader(plain, type, token, srcVirt, dstVirt, srcPort, dstPort, length)) return false;
    if (length && payload) memcpy(plain + kHeaderSize, payload, length);
    uint8_t frame[ngs::kMaxFrame];
    size_t frameLength = 0;
    if (!peer.channel.Seal(plain, kHeaderSize + length, frame, frameLength)) return false;
    int sent = sendto(peer.socket, (const char*)frame, (int)frameLength, 0,
                      (const sockaddr*)&peer.server, sizeof(peer.server));
    return sent == (int)frameLength;
}

static bool ReceivePlain(Peer& peer, RelayHeader& header, uint8_t* payload, size_t capacity,
                         int timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        uint8_t buffer[ngs::kMaxFrame];
        int received = recv(peer.socket, (char*)buffer, sizeof(buffer), 0);
        if (received <= 0) continue;
        if (!ngs::IsSecureFrame(buffer, (size_t)received)) continue;
        if (buffer[5] != ngs::FrameData) continue;
        uint8_t plain[ngs::kMaxPlain];
        size_t plainLength = 0;
        if (!peer.channel.Open(buffer, (size_t)received, plain, plainLength)) continue;
        if (!DecodeHeader(plain, plainLength, header)) continue;
        size_t copy = header.length < capacity ? header.length : capacity;
        memcpy(payload, plain + kHeaderSize, copy);
        return true;
    }
    return false;
}

// Waits for a specific message type, discarding peer-list updates in between.
static bool ReceiveType(Peer& peer, uint8_t wanted, RelayHeader& header, uint8_t* payload,
                        size_t capacity, int timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        int remaining = static_cast<int>(deadline - GetTickCount64());
        if (remaining <= 0) break;
        if (!ReceivePlain(peer, header, payload, capacity, remaining)) break;
        if (header.type == wanted) return true;
    }
    return false;
}

static bool OpenPeer(Peer& peer, int port) {
    peer.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (peer.socket == INVALID_SOCKET) return false;
    DWORD timeout = 300;
    setsockopt(peer.socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    peer.server.sin_family = AF_INET;
    peer.server.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &peer.server.sin_addr);
    return Handshake(peer);
}

static bool JoinRoom(Peer& peer, const char* room, const char* name) {
    uint8_t hello[fr::kRoomLength + 8 + fr::kNameLength];
    size_t helloLength = EncodeHello(hello, room, 0, name);
    if (!SendPlain(peer, MessageType::Hello, 0, 0, 0, 0, 0, hello,
                   (uint16_t)helloLength)) {
        return false;
    }
    RelayHeader header{};
    uint8_t payload[fr::kMaxPayload];
    if (!ReceivePlain(peer, header, payload, sizeof(payload), 3000)) return false;
    if (header.type != MessageType::HelloAck) return false;
    uint32_t subnet = 0, mask = 0;
    std::string roomName;
    uint32_t ips[fr::kMaxPeers] = {};
    std::string names[fr::kMaxPeers];
    int count = 0;
    if (!DecodeHelloAck(payload, header.length, subnet, mask, roomName, ips, names, count))
        return false;
    peer.token = header.token;
    peer.virtualIp = header.srcVirt;
    return peer.virtualIp != 0;
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : kDefaultPort;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    Peer a, b;
    CHECK(OpenPeer(a, port));
    CHECK(JoinRoom(a, "SECURETEST", "peerA"));
    CHECK(OpenPeer(b, port));
    CHECK(JoinRoom(b, "SECURETEST", "peerB"));

    // peerA must know about peerB through the peer list pushed by the server.
    {
        RelayHeader header{};
        uint8_t payload[fr::kMaxPayload];
        CHECK(ReceiveType(a, MessageType::PeerList, header, payload, sizeof(payload), 2000));
    }

    const char message[] = "encrypted relay payload";
    CHECK(SendPlain(a, MessageType::Data, a.token, 0, b.virtualIp, 1234, 5678,
                    (const uint8_t*)message, (uint16_t)strlen(message)));

    RelayHeader header{};
    uint8_t payload[fr::kMaxPayload];
    bool received = ReceiveType(b, MessageType::Data, header, payload, sizeof(payload), 3000);
    CHECK(received);
    CHECK(header.type == MessageType::Data);
    CHECK(header.length == strlen(message));
    CHECK(received && memcmp(payload, message, strlen(message)) == 0);

    SendPlain(a, MessageType::Bye, a.token, 0, 0, 0, 0, nullptr, 0);
    SendPlain(b, MessageType::Bye, b.token, 0, 0, 0, 0, nullptr, 0);
    closesocket(a.socket);
    closesocket(b.socket);
    WSACleanup();

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
