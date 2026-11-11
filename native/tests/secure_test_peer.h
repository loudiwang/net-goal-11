// Shared helper for the manual test clients: performs the encrypted handshake
// with the relay server and sends/receives plaintext protocol datagrams.
#pragma once

#include <string.h>

#include "../common/secure.h"

#ifdef _WIN32
#else
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

struct SecureTestPeer {
    int socket = -1;
    sockaddr_in server = {};
    ngs::Channel channel = {};

    static uint64_t NowMs() {
#ifdef _WIN32
        return (uint64_t)GetTickCount64();
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
    }

    bool Open(const char* host, int port, int timeoutMs = 3000) {
#ifdef _WIN32
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        DWORD timeout = (DWORD)timeoutMs / 2;
        socket = (int)::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == (int)INVALID_SOCKET) return false;
        setsockopt((SOCKET)socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket < 0) return false;
        timeval timeout{timeoutMs / 2000, (timeoutMs % 2000) * 500};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        server.sin_family = AF_INET;
        server.sin_port = htons((unsigned short)port);
        inet_pton(AF_INET, host, &server.sin_addr);
        return Handshake(timeoutMs);
    }

    void Close() {
        if (socket >= 0) {
#ifdef _WIN32
            closesocket((SOCKET)socket);
#else
            ::close(socket);
#endif
        }
        socket = -1;
    }

    bool Handshake(int timeoutMs) {
        channel.BeginClientHandshake(NowMs());
        uint8_t init[ngs::kMaxFrame];
        size_t initLength = 0;
        channel.BuildInit(init, initLength);

        uint64_t deadline = NowMs() + (uint64_t)timeoutMs;
        uint64_t nextSend = 0;
        uint8_t buffer[ngs::kMaxFrame];
        while (NowMs() < deadline) {
            if (NowMs() >= nextSend) {
                sendto(socket, (const char*)init, (int)initLength, 0,
                       (const sockaddr*)&server, sizeof(server));
                nextSend = NowMs() + 300;
            }
            int received = (int)recv(socket, (char*)buffer, sizeof(buffer), 0);
            if (received <= 0) continue;
            if (!ngs::IsSecureFrame(buffer, (size_t)received)) continue;
            if (buffer[5] != ngs::FrameReply) continue;
            if (channel.HandleReply(buffer, (size_t)received, NowMs())) return true;
        }
        return false;
    }

    bool Send(const uint8_t* plain, size_t length) {
        uint8_t frame[ngs::kMaxFrame];
        size_t frameLength = 0;
        if (!channel.Seal(plain, length, frame, frameLength)) return false;
        int sent = (int)sendto(socket, (const char*)frame, (int)frameLength, 0,
                               (const sockaddr*)&server, sizeof(server));
        return sent == (int)frameLength;
    }

    // Returns the decrypted datagram length, or -1 on timeout.
    int Receive(uint8_t* out, size_t capacity) {
        uint8_t buffer[ngs::kMaxFrame];
        for (;;) {
            int received = (int)recv(socket, (char*)buffer, sizeof(buffer), 0);
            if (received <= 0) return -1;
            if (!ngs::IsSecureFrame(buffer, (size_t)received)) continue;
            if (buffer[5] != ngs::FrameData) continue;
            size_t plainLength = 0;
            if (!channel.Open(buffer, (size_t)received, out, plainLength)) continue;
            if (plainLength > capacity) continue;
            return (int)plainLength;
        }
    }
};
