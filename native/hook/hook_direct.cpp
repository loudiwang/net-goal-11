// Net Goal 11.Hook.dll - 32-bit Winsock hook injected into the game process.
// Intercepts UDP send/recv to/from the virtual LAN and relays through the
// Net Goal 11 client GUI over a localhost IPC socket. No kernel driver.
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include "../common/protocol.h"
#include "../common/secure.h"

#ifndef SIO_ROUTING_INTERFACE_QUERY
#define SIO_ROUTING_INTERFACE_QUERY 0xC8000014
#endif
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000C
#endif
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000C
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "iphlpapi.lib")

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct HookConfig
{
    int guiPort = 45889;
    int dllPort = 45890;
    uint32_t subnet = 0;
    uint32_t mask = 0;
    uint32_t localIp = 0;
    uint32_t selfVirt = 0;   // relay-assigned virtual IP this game should believe it owns
    int logLevel = 1;
    std::wstring logPath;

    // Direct relay mode: the hook itself talks to the VPS, so game packets do
    // not make a round trip through the GUI process.
    bool direct = false;
    std::string serverHost = "127.0.0.1";
    int serverPort = fr::kDefaultPort;
    std::string room = "OL";
    std::string name = "Player";
    uint64_t passwordHash = 0;
};

static HookConfig g_cfg;
static HMODULE g_hModule = nullptr;
static std::wstring g_moduleDir;
static std::wstring g_iniPath;

static std::string IpToString(uint32_t ip);

static std::string IpToString(uint32_t ip);

static uint32_t IpFromString(const char* text)
{
    if (!text || !*text) return 0;
    uint32_t value = inet_addr(text);
    return value == INADDR_NONE ? 0 : value;
}

static std::string WideToAnsi(const std::wstring& text)
{
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

static std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring IniString(const wchar_t* key, const wchar_t* fallback)
{
    wchar_t buffer[1024] = {};
    GetPrivateProfileStringW(L"hook", key, fallback, buffer, (DWORD)(sizeof(buffer) / sizeof(buffer[0])), g_iniPath.c_str());
    return buffer;
}

static int IniInt(const wchar_t* key, int fallback)
{
    return (int)GetPrivateProfileIntW(L"hook", key, fallback, g_iniPath.c_str());
}

static std::wstring GetModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring value(path);
    size_t pos = value.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : value.substr(0, pos);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;

static void LogInit()
{
    if (g_cfg.logPath.empty())
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        g_cfg.logPath = std::wstring(temp) + L"Net Goal 11 Hook.log";
    }
    g_logFile = _wfsopen(g_cfg.logPath.c_str(), L"a", _SH_DENYNO);
}

static void LogMessage(int level, const char* format, ...)
{
    if (level > g_cfg.logLevel) return;
    char body[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(body, sizeof(body), format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2304];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d][%lu][%s] %s\r\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             (unsigned long)GetCurrentThreadId(),
             level == 0 ? "ERR" : (level == 1 ? "INF" : "DBG"),
             body);

    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logFile)
        {
            fputs(line, g_logFile);
            fflush(g_logFile);
        }
    }
    OutputDebugStringA(line);
}

// Guard the call itself, not only the body of LogMessage: several call sites
// build IpToString(...) arguments, which must not allocate on the packet path.
#define LogError(...) do { if (g_cfg.logLevel >= 0) LogMessage(0, __VA_ARGS__); } while (0)
#define LogInfo(...)  do { if (g_cfg.logLevel >= 1) LogMessage(1, __VA_ARGS__); } while (0)
#define LogDebug(...) do { if (g_cfg.logLevel >= 2) LogMessage(2, __VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct IpcHeader
{
    uint32_t magic;
    uint8_t type;
    uint8_t kind;
    uint16_t flags;
    uint32_t dstVirt;
    uint16_t srcPort;
    uint16_t dstPort;
    uint16_t len;
    uint16_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(IpcHeader) == 20, "IpcHeader must be 20 bytes");

static const uint32_t IpcMagic = 0x31485246; // "FRH1"
enum IpcType : uint8_t
{
    IpcReady = 1,
    IpcCapture = 2,
    IpcInject = 3,
    IpcConfig = 4,
    IpcLog = 5,
    IpcRelayConfig = 6,
    IpcStatus = 7,
    IpcPeers = 8,
    IpcStats = 9,
    IpcRelayReady = 10,
    IpcRelayStop = 11,
    IpcChat = 12,
};
enum CaptureKind : uint8_t
{
    KindData = 1,
    KindBroadcast = 2,
};

static SOCKET g_ipcSocket = INVALID_SOCKET;
static sockaddr_in g_guiAddress{};
static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_hooksInstalled{ false };
static bool g_wsaStarted = false;

static bool SendIpc(uint8_t type, uint8_t kind, uint32_t dstVirt,
                    uint16_t srcPort, uint16_t dstPort,
                    const void* payload, uint16_t length)
{
    if (g_ipcSocket == INVALID_SOCKET) return false;
    if (length > 1400) return false;

    uint8_t buffer[sizeof(IpcHeader) + 1400];
    IpcHeader header{};
    header.magic = IpcMagic;
    header.type = type;
    header.kind = kind;
    header.dstVirt = dstVirt;
    header.srcPort = srcPort;
    header.dstPort = dstPort;
    header.len = length;
    memcpy(buffer, &header, sizeof(header));
    if (length > 0 && payload)
        memcpy(buffer + sizeof(header), payload, length);

    int sent = sendto(g_ipcSocket, (const char*)buffer, (int)(sizeof(header) + length), 0,
                      (const sockaddr*)&g_guiAddress, sizeof(g_guiAddress));
    return sent == (int)(sizeof(header) + length);
}

static void SendReady()
{
    uint8_t payload[8];
    uint32_t pid = GetCurrentProcessId();
    uint16_t version = 1;
    memcpy(payload, &pid, 4);
    memcpy(payload + 4, &version, 2);
    SendIpc(IpcReady, 0, 0, 0, 0, payload, 6);
}

// ---------------------------------------------------------------------------
// Direct relay client (inside the game process)
// ---------------------------------------------------------------------------
static void InjectPayload(uint8_t kind, uint32_t dstVirt, uint16_t srcPort,
                          uint16_t dstPort, const uint8_t* payload, uint16_t length);
static void StartRelay();
static void StopRelay();

static std::mutex g_relayMutex;
static std::recursive_mutex g_relayControlMutex;
static SOCKET g_relaySocket = INVALID_SOCKET;
static std::atomic<uint64_t> g_relayToken{0};
static std::atomic<bool> g_relayConnected{false};
static std::atomic<bool> g_relayRunning{false};
static HANDLE g_relayThread = nullptr;
static std::atomic<uint64_t> g_upPackets{0};
static std::atomic<uint64_t> g_downPackets{0};
static std::atomic<uint64_t> g_upBytes{0};
static std::atomic<uint64_t> g_downBytes{0};

// Encrypted transport to the relay server (X25519 + XSalsa20-Poly1305).
static ngs::Channel g_relayChannel;
static std::mutex g_relaySecureMutex;

// Encrypt one plaintext protocol datagram and send it on the relay socket.
// Returns false when the secure channel is not established yet.
static bool RelaySendSecure(SOCKET s, const uint8_t* plain, size_t length)
{
    if (s == INVALID_SOCKET || !plain) return false;
    uint8_t frame[ngs::kMaxFrame];
    size_t frameLength = 0;
    {
        std::lock_guard<std::mutex> lock(g_relaySecureMutex);
        if (!g_relayChannel.ready) return false;
        if (!g_relayChannel.Seal(plain, length, frame, frameLength)) return false;
    }
    return send(s, (const char*)frame, (int)frameLength, 0) == (int)frameLength;
}

static void SendRelayStatus(fr::RelayState state, const char* text)
{
    fr::IpcStatusPayload payload{};
    payload.state = static_cast<uint8_t>(state);
    payload.selfVirt = g_cfg.selfVirt;
    payload.subnet = g_cfg.subnet;
    payload.mask = g_cfg.mask;
    strncpy(payload.text, text ? text : "", sizeof(payload.text) - 1);
    SendIpc(IpcStatus, 0, 0, 0, 0, &payload, sizeof(payload));
}

static void SendRelayPeers(const uint8_t* payload, uint16_t length)
{
    if (payload && length) SendIpc(IpcPeers, 0, 0, 0, 0, payload, length);
}

static void SendRelayStats()
{
    fr::IpcStatsPayload payload{};
    payload.upPackets = g_upPackets.load(std::memory_order_relaxed);
    payload.downPackets = g_downPackets.load(std::memory_order_relaxed);
    payload.upBytes = g_upBytes.load(std::memory_order_relaxed);
    payload.downBytes = g_downBytes.load(std::memory_order_relaxed);
    SendIpc(IpcStats, 0, 0, 0, 0, &payload, sizeof(payload));
}

static bool RelaySendData(uint32_t dstVirt, uint16_t srcPort, uint16_t dstPort,
                          const uint8_t* payload, uint16_t length)
{
    uint8_t packet[fr::kMaxDatagram];
    std::lock_guard<std::mutex> lock(g_relayMutex);
    if (g_relaySocket == INVALID_SOCKET || !g_relayConnected.load(std::memory_order_relaxed))
        return false;
    if (!fr::EncodeHeader(packet, fr::MessageType::Data, g_relayToken.load(std::memory_order_relaxed),
                          0, dstVirt, srcPort, dstPort, length))
        return false;
    memcpy(packet + fr::kHeaderSize, payload, length);
    return RelaySendSecure(g_relaySocket, packet, fr::kHeaderSize + length);
}

static bool RelaySendBroadcast(uint16_t srcPort, const uint8_t* payload, uint16_t length)
{
    uint8_t packet[fr::kMaxDatagram];
    std::lock_guard<std::mutex> lock(g_relayMutex);
    if (g_relaySocket == INVALID_SOCKET || !g_relayConnected.load(std::memory_order_relaxed))
        return false;
    if (!fr::EncodeHeader(packet, fr::MessageType::Broadcast, g_relayToken.load(std::memory_order_relaxed),
                          0, 0, srcPort, fr::kDiscoveryPort, length))
        return false;
    memcpy(packet + fr::kHeaderSize, payload, length);
    return RelaySendSecure(g_relaySocket, packet, fr::kHeaderSize + length);
}

static bool RelaySendPing()
{
    uint8_t packet[fr::kHeaderSize];
    std::lock_guard<std::mutex> lock(g_relayMutex);
    if (g_relaySocket == INVALID_SOCKET || !g_relayConnected.load(std::memory_order_relaxed))
        return false;
    if (!fr::EncodeHeader(packet, fr::MessageType::Ping, g_relayToken.load(std::memory_order_relaxed),
                          0, 0, 0, 0, 0))
        return false;
    return RelaySendSecure(g_relaySocket, packet, sizeof(packet));
}

static bool RelaySendChat(const std::string& text)
{
    if (text.empty() || text.size() > fr::kMaxChatText) return false;
    uint8_t packet[fr::kMaxDatagram];
    std::lock_guard<std::mutex> lock(g_relayMutex);
    if (g_relaySocket == INVALID_SOCKET || !g_relayConnected.load(std::memory_order_relaxed))
        return false;
    if (!fr::EncodeHeader(packet, fr::MessageType::Chat,
                          g_relayToken.load(std::memory_order_relaxed),
                          0, 0, 0, 0, (uint16_t)text.size()))
        return false;
    memcpy(packet + fr::kHeaderSize, text.data(), text.size());
    return RelaySendSecure(g_relaySocket, packet, fr::kHeaderSize + text.size());
}

static bool ResolveServer(sockaddr_in& address)
{
    char portText[16];
    snprintf(portText, sizeof(portText), "%d", g_cfg.serverPort);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* result = nullptr;
    int rc = getaddrinfo(g_cfg.serverHost.c_str(), portText, &hints, &result);
    if (rc != 0 || !result)
    {
        LogError("relay resolve %s:%s failed: %d", g_cfg.serverHost.c_str(), portText, rc);
        return false;
    }
    memcpy(&address, result->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(result);
    return true;
}

static void CloseRelaySocket(SOCKET s)
{
    std::lock_guard<std::mutex> lock(g_relayMutex);
    if (g_relaySocket == s)
        g_relaySocket = INVALID_SOCKET;
    g_relayConnected.store(false, std::memory_order_relaxed);
    if (s != INVALID_SOCKET)
        closesocket(s);
}

static DWORD WINAPI RelayThread(LPVOID)
{
    if (!g_cfg.direct)
    {
        g_relayRunning.store(false);
        return 0;
    }

    SendRelayStatus(fr::RelayConnecting, "Connecting to the relay server...");
    sockaddr_in server{};
    if (!ResolveServer(server))
    {
        SendRelayStatus(fr::RelayError, "Cannot resolve the relay server address");
        g_relayRunning.store(false);
        return 0;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        SendRelayStatus(fr::RelayError, "Failed to create the relay UDP socket");
        g_relayRunning.store(false);
        return 0;
    }

    int bufferSize = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&bufferSize, sizeof(bufferSize));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&bufferSize, sizeof(bufferSize));
    int tos = 0xB8;
    setsockopt(s, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof(tos));
    BOOL noReset = FALSE;
    DWORD ignored = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), nullptr, 0, &ignored, nullptr, nullptr);

    if (connect(s, (const sockaddr*)&server, sizeof(server)) != 0)
    {
        LogError("relay connect %s:%d failed: %d", g_cfg.serverHost.c_str(), g_cfg.serverPort, WSAGetLastError());
        SendRelayStatus(fr::RelayError, "Failed to connect to the relay server");
        closesocket(s);
        g_relayRunning.store(false);
        return 0;
    }

    sockaddr_in local{};
    int localLen = sizeof(local);
    if (getsockname(s, (sockaddr*)&local, &localLen) == 0 && local.sin_addr.s_addr != 0)
        g_cfg.localIp = local.sin_addr.s_addr;

    {
        std::lock_guard<std::mutex> lock(g_relayMutex);
        g_relaySocket = s;
    }
    LogInfo("relay connecting to %s:%d local=%s", g_cfg.serverHost.c_str(), g_cfg.serverPort,
            IpToString(g_cfg.localIp).c_str());

    // Encrypted transport: exchange ephemeral X25519 keys with the relay.  The
    // server answers only to a valid handshake, so a failed handshake means the
    // relay is unreachable or running an incompatible version.
    {
        std::lock_guard<std::mutex> lock(g_relaySecureMutex);
        g_relayChannel.BeginClientHandshake(GetTickCount64());
    }
    {
        uint8_t init[ngs::kMaxFrame];
        size_t initLength = 0;
        {
            std::lock_guard<std::mutex> lock(g_relaySecureMutex);
            g_relayChannel.BuildInit(init, initLength);
        }
        bool established = false;
        ULONGLONG deadline = GetTickCount64() + 3000;
        ULONGLONG nextSend = 0;
        while (g_relayRunning.load(std::memory_order_relaxed) && GetTickCount64() < deadline) {
            if (GetTickCount64() >= nextSend) {
                send(s, (const char*)init, (int)initLength, 0);
                nextSend = GetTickCount64() + 400;
            }
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(s, &readSet);
            timeval timeout{0, 200000};
            int ready = select(0, &readSet, nullptr, nullptr, &timeout);
            if (ready <= 0) continue;
            uint8_t buffer[ngs::kMaxFrame];
            int received = recv(s, (char*)buffer, sizeof(buffer), 0);
            if (received <= 0) continue;
            if (!ngs::IsSecureFrame(buffer, (size_t)received)) continue;
            if (buffer[5] != ngs::FrameReply) continue;
            std::lock_guard<std::mutex> lock(g_relaySecureMutex);
            if (g_relayChannel.HandleReply(buffer, (size_t)received, GetTickCount64())) {
                established = true;
                break;
            }
        }
        if (!established) {
            SendRelayStatus(fr::RelayError, "Encrypted handshake with the relay server failed");
            LogError("relay encrypted handshake timeout");
            CloseRelaySocket(s);
            g_relayRunning.store(false);
            return 0;
        }
    }
    LogInfo("relay encrypted channel established");

    uint8_t helloPayload[fr::kRoomLength + 8 + fr::kNameLength];
    size_t helloLength = fr::EncodeHello(helloPayload, g_cfg.room.c_str(), g_cfg.passwordHash, g_cfg.name);
    uint8_t helloPacket[fr::kHeaderSize + sizeof(helloPayload)];
    fr::EncodeHeader(helloPacket, fr::MessageType::Hello, 0, 0, 0, 0, 0,
                     (uint16_t)helloLength);
    memcpy(helloPacket + fr::kHeaderSize, helloPayload, helloLength);

    auto now = std::chrono::steady_clock::now();
    auto nextHello = now;
    auto nextPing = now;
    auto nextStats = now;
    auto lastPong = now;
    bool acknowledged = false;
    int helloAttempts = 0;

    while (g_relayRunning.load(std::memory_order_relaxed))
    {
        now = std::chrono::steady_clock::now();
        if (!acknowledged && now >= nextHello)
        {
            if (helloAttempts >= 8)
            {
                SendRelayStatus(fr::RelayError, "Timed out connecting to the relay server");
                LogError("relay hello timeout");
                break;
            }
            ++helloAttempts;
            if (!RelaySendSecure(s, helloPacket, fr::kHeaderSize + helloLength))
            {
                LogError("relay hello send failed: %d", WSAGetLastError());
                break;
            }
            nextHello = now + std::chrono::seconds(1);
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s, &readSet);
        timeval timeout{0, 200000};
        int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0)
        {
            int error = WSAGetLastError();
            if (error == WSAENOTSOCK) break;
            if (error != WSAEINTR) LogError("relay select failed: %d", error);
            continue;
        }

        if (ready > 0)
        {
            uint8_t buffer[ngs::kMaxFrame];
            int received = recv(s, (char*)buffer, sizeof(buffer), 0);
            if (received < 0)
            {
                int error = WSAGetLastError();
                if (error == WSAECONNRESET || error == WSAEINTR) continue;
                if (error == WSAENOTSOCK) break;
                LogError("relay recv failed: %d", error);
                break;
            }

            uint8_t plain[ngs::kMaxPlain];
            size_t plainLength = 0;
            {
                std::lock_guard<std::mutex> lock(g_relaySecureMutex);
                if (!g_relayChannel.Open(buffer, (size_t)received, plain, plainLength)) continue;
            }

            fr::RelayHeader header{};
            if (plainLength >= fr::kHeaderSize &&
                fr::DecodeHeader(plain, plainLength, header))
            {
                uint64_t token = g_relayToken.load(std::memory_order_relaxed);
                switch (header.type)
                {
                case fr::MessageType::HelloAck:
                {
                    uint32_t subnet = 0, mask = 0;
                    std::string room;
                    uint32_t ips[fr::kMaxPeers] = {};
                    std::string names[fr::kMaxPeers];
                    int count = 0;
                    if (!fr::DecodeHelloAck(plain + fr::kHeaderSize, header.length, subnet, mask,
                                            room, ips, names, count))
                    {
                        SendRelayStatus(fr::RelayError, "The server returned an invalid HelloAck");
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_relayMutex);
                        g_relayToken.store(header.token, std::memory_order_relaxed);
                        g_relayConnected.store(true, std::memory_order_relaxed);
                    }
                    g_cfg.subnet = subnet & mask;
                    g_cfg.mask = mask;
                    g_cfg.selfVirt = header.srcVirt;
                    acknowledged = true;
                    lastPong = now;
                    nextPing = now + std::chrono::seconds(5);
                    LogInfo("relay joined room=%s self=%s subnet=%s mask=%s token=%016llx",
                            room.c_str(), IpToString(g_cfg.selfVirt).c_str(),
                            IpToString(g_cfg.subnet).c_str(), IpToString(g_cfg.mask).c_str(),
                            (unsigned long long)header.token);
                    char status[160];
                    snprintf(status, sizeof(status), "Relay connected, virtual IP %s",
                             IpToString(g_cfg.selfVirt).c_str());
                    SendRelayStatus(fr::RelayConnected, status);
                    uint16_t peerLength = (uint16_t)(header.length - (8 + fr::kRoomLength));
                    SendRelayPeers(plain + fr::kHeaderSize + 8 + fr::kRoomLength, peerLength);
                    uint32_t self = g_cfg.selfVirt;
                    SendIpc(IpcRelayReady, 0, 0, 0, 0, &self, sizeof(self));
                    break;
                }
                case fr::MessageType::PeerList:
                    if (header.token == token)
                        SendRelayPeers(plain + fr::kHeaderSize, header.length);
                    break;
                case fr::MessageType::Data:
                    if (header.token == token && header.length > 0)
                    {
                        InjectPayload(KindData, header.srcVirt, header.srcPort, header.dstPort,
                                      plain + fr::kHeaderSize, header.length);
                        g_downPackets.fetch_add(1, std::memory_order_relaxed);
                        g_downBytes.fetch_add(header.length, std::memory_order_relaxed);
                    }
                    break;
                case fr::MessageType::Broadcast:
                    if (header.token == token && header.length > 0)
                    {
                        InjectPayload(KindBroadcast, header.srcVirt, header.srcPort, fr::kDiscoveryPort,
                                      plain + fr::kHeaderSize, header.length);
                        g_downPackets.fetch_add(1, std::memory_order_relaxed);
                        g_downBytes.fetch_add(header.length, std::memory_order_relaxed);
                    }
                    break;
                case fr::MessageType::Chat:
                    if (header.token == token && header.length > 0)
                        SendIpc(IpcChat, 0, 0, 0, 0, plain + fr::kHeaderSize, header.length);
                    break;
                case fr::MessageType::Pong:
                    if (header.token == token) lastPong = now;
                    break;
                case fr::MessageType::Error:
                {
                    std::string text((const char*)plain + fr::kHeaderSize, header.length);
                    LogError("relay server error: %s", text.c_str());
                    SendRelayStatus(fr::RelayError, text.c_str());
                    break;
                }
                default:
                    break;
                }
            }
        }

        now = std::chrono::steady_clock::now();
        if (acknowledged)
        {
            if (now >= nextPing)
            {
                RelaySendPing();
                nextPing = now + std::chrono::seconds(5);
            }
            if (now - lastPong > std::chrono::seconds(25))
            {
                SendRelayStatus(fr::RelayError, "Relay server heartbeat timed out");
                LogError("relay heartbeat timeout");
                break;
            }
        }
        if (now >= nextStats)
        {
            SendRelayStats();
            nextStats = now + std::chrono::seconds(1);
        }
    }

    if (acknowledged)
    {
        uint8_t bye[fr::kHeaderSize];
        fr::EncodeHeader(bye, fr::MessageType::Bye, g_relayToken.load(std::memory_order_relaxed),
                         0, 0, 0, 0, 0);
        RelaySendSecure(s, bye, sizeof(bye));
        char status[160];
        snprintf(status, sizeof(status), "Relay disconnected, virtual IP %s",
                 IpToString(g_cfg.selfVirt).c_str());
        SendRelayStatus(fr::RelayDisconnected, status);
    }
    CloseRelaySocket(s);
    g_relayRunning.store(false, std::memory_order_relaxed);
    LogInfo("relay thread stopped");
    return 0;
}

static void StartRelay()
{
    std::lock_guard<std::recursive_mutex> control(g_relayControlMutex);
    if (!g_cfg.direct) return;
    StopRelay();
    g_relayRunning.store(true, std::memory_order_relaxed);
    g_relayThread = CreateThread(nullptr, 0, RelayThread, nullptr, 0, nullptr);
    if (!g_relayThread)
    {
        g_relayRunning.store(false, std::memory_order_relaxed);
        SendRelayStatus(fr::RelayError, "Failed to create the relay thread");
    }
}

static void StopRelay()
{
    std::lock_guard<std::recursive_mutex> control(g_relayControlMutex);
    g_relayRunning.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_relayMutex);
        if (g_relaySocket != INVALID_SOCKET)
        {
            if (g_relayConnected.load(std::memory_order_relaxed))
            {
                uint8_t bye[fr::kHeaderSize];
                fr::EncodeHeader(bye, fr::MessageType::Bye, g_relayToken.load(std::memory_order_relaxed),
                                 0, 0, 0, 0, 0);
                RelaySendSecure(g_relaySocket, bye, sizeof(bye));
            }
            closesocket(g_relaySocket);
            g_relaySocket = INVALID_SOCKET;
        }
        g_relayConnected.store(false, std::memory_order_relaxed);
    }
    if (g_relayThread)
    {
        WaitForSingleObject(g_relayThread, 2000);
        CloseHandle(g_relayThread);
        g_relayThread = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Address / packet helpers
// ---------------------------------------------------------------------------
static bool IsVirtualIp(uint32_t ip)
{
    if (g_cfg.mask == 0) return false;
    return (ip & g_cfg.mask) == (g_cfg.subnet & g_cfg.mask);
}

static bool IsBroadcastIp(uint32_t ip)
{
    // On little-endian the last dotted octet is the high byte of the
    // network-order uint32 (e.g. 192.168.1.255 -> 0xFF01A8C0).
    return ip == INADDR_BROADCAST || ((ip >> 24) == 0xFF);
}

static std::string IpToString(uint32_t ip)
{
    in_addr address{};
    address.s_addr = ip;
    char buffer[INET_ADDRSTRLEN] = {};
    InetNtopA(AF_INET, &address, buffer, sizeof(buffer));
    return buffer;
}

// ---------------------------------------------------------------------------
// Virtual adapter spoofing
//
// The game decides "who am I" from the adapter list.  When the relay hands the
// game a 10.213.77.x peer but the machine's real adapter is 192.168.x.x the
// LAN code refuses to treat the remote as a human player.  Present the
// relay-assigned virtual IP as the only IPv4 address the game can see.
// ---------------------------------------------------------------------------
static uint8_t PrefixFromMask(uint32_t mask)
{
    uint32_t networkMask = htonl(mask);
    uint8_t prefix = 0;
    while (networkMask & 0x80000000u) { ++prefix; networkMask <<= 1; }
    return prefix;
}

static void SpoofAdapterAddresses(PIP_ADAPTER_ADDRESSES list)
{
    uint8_t prefix = PrefixFromMask(g_cfg.mask);
    int rewritten = 0;
    for (PIP_ADAPTER_ADDRESSES adapter = list; adapter; adapter = adapter->Next)
    {
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
             unicast; unicast = unicast->Next)
        {
            sockaddr* address = unicast->Address.lpSockaddr;
            if (!address || address->sa_family != AF_INET) continue;
            ((sockaddr_in*)address)->sin_addr.s_addr = g_cfg.selfVirt;
            unicast->OnLinkPrefixLength = prefix;
            ++rewritten;
        }
    }
    LogInfo("adapter spoof: %d IPv4 address(es) -> %s/%u", rewritten,
            IpToString(g_cfg.selfVirt).c_str(), (unsigned)prefix);
}

static void SpoofAdapterInfo(PIP_ADAPTER_INFO info)
{
    std::string ip = IpToString(g_cfg.selfVirt);
    std::string mask = IpToString(g_cfg.mask);
    int rewritten = 0;
    for (PIP_ADAPTER_INFO adapter = info; adapter; adapter = adapter->Next)
    {
        IP_ADDR_STRING* address = &adapter->IpAddressList;
        if (!address->IpAddress.String[0]) continue;
        snprintf(address->IpAddress.String, sizeof(address->IpAddress.String), "%s", ip.c_str());
        snprintf(address->IpMask.String, sizeof(address->IpMask.String), "%s", mask.c_str());
        address->Context = 0;
        address->Next = nullptr;
        ++rewritten;
    }
    LogInfo("adapter info spoof: %d IPv4 address(es) -> %s", rewritten, ip.c_str());
}

static void SpoofAddressList(PSOCKET_ADDRESS_LIST list)
{
    if (!list || list->iAddressCount <= 0) return;
    SOCKET_ADDRESS chosen{};
    bool found = false;
    for (int i = 0; i < list->iAddressCount; ++i)
    {
        sockaddr* address = list->Address[i].lpSockaddr;
        if (address && address->sa_family == AF_INET)
        {
            chosen = list->Address[i];
            found = true;
            break;
        }
    }
    if (!found) return;
    ((sockaddr_in*)chosen.lpSockaddr)->sin_addr.s_addr = g_cfg.selfVirt;
    list->Address[0] = chosen;
    list->iAddressCount = 1;
    LogInfo("address list spoof -> %s", IpToString(g_cfg.selfVirt).c_str());
}

static uint16_t GetLocalPort(SOCKET s)
{
    sockaddr_in address{};
    int length = sizeof(address);
    if (getsockname(s, (sockaddr*)&address, &length) == 0)
        return ntohs(address.sin_port);
    return 0;
}

static void SendCapture(uint32_t dstVirt, uint16_t srcPort, uint16_t dstPort,
                        const char* data, int length, uint8_t kind)
{
    if (!data || length <= 0) return;
    if (length > 1400)
    {
        LogDebug("drop oversized capture len=%d", length);
        return;
    }
    if (kind == KindBroadcast) dstVirt = 0;

    if (g_cfg.direct)
    {
        bool ok = (kind == KindBroadcast)
            ? RelaySendBroadcast(srcPort, (const uint8_t*)data, (uint16_t)length)
            : RelaySendData(dstVirt, srcPort, dstPort, (const uint8_t*)data, (uint16_t)length);
        if (ok)
        {
            g_upPackets.fetch_add(1, std::memory_order_relaxed);
            g_upBytes.fetch_add((uint64_t)length, std::memory_order_relaxed);
            LogDebug("relay capture kind=%u dst=%s src=%u dstPort=%u len=%d",
                     kind, IpToString(dstVirt).c_str(), srcPort, dstPort, length);
        }
        else
        {
            LogDebug("relay capture dropped (not connected) kind=%u len=%d", kind, length);
        }
        return;
    }

    bool ok = SendIpc(IpcCapture, kind, dstVirt, srcPort, dstPort, data, (uint16_t)length);
    if (!ok)
        LogDebug("capture ipc failed kind=%u dst=%s:%u len=%d",
                 kind, IpToString(dstVirt).c_str(), dstPort, length);
    else
        LogDebug("capture kind=%u dst=%s src=%u dstPort=%u len=%d",
                 kind, IpToString(dstVirt).c_str(), srcPort, dstPort, length);
}

// ---------------------------------------------------------------------------
// Virtual peer sockets and source-address rewriting
// ---------------------------------------------------------------------------
struct PeerKey
{
    uint32_t ip = 0;
    uint16_t port = 0;
};

static std::mutex g_mapMutex;
static std::unordered_map<uint64_t, SOCKET> g_injectSockets; // (virtIp << 16 | srcPort) -> socket
static std::unordered_map<uint16_t, PeerKey> g_localPorts;   // local source port -> peer
static std::unordered_map<SOCKET, PeerKey> g_socketPeers;    // connected game socket -> peer

static SOCKET GetInjectSocket(uint32_t virtIp, uint16_t srcPort)
{
    uint64_t key = ((uint64_t)virtIp << 16) | srcPort;
    std::lock_guard<std::mutex> lock(g_mapMutex);
    auto existing = g_injectSockets.find(key);
    if (existing != g_injectSockets.end())
        return existing->second;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        LogError("socket() for inject failed: %d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(s, (sockaddr*)&local, sizeof(local)) != 0)
    {
        LogError("bind() for inject failed: %d", WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }

    int length = sizeof(local);
    if (getsockname(s, (sockaddr*)&local, &length) != 0)
    {
        LogError("getsockname() for inject failed: %d", WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }

    uint16_t localPort = ntohs(local.sin_port);
    g_localPorts[localPort] = PeerKey{ virtIp, srcPort };
    g_injectSockets[key] = s;
    LogDebug("inject socket %s:%u -> local port %u", IpToString(virtIp).c_str(), srcPort, localPort);
    return s;
}

static void RewriteFrom(sockaddr* from, int* fromLen)
{
    if (!from || !fromLen || *fromLen < (int)sizeof(sockaddr_in)) return;
    sockaddr_in* address = (sockaddr_in*)from;
    if (address->sin_family != AF_INET) return;

    uint16_t localPort = ntohs(address->sin_port);
    PeerKey peer{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = g_localPorts.find(localPort);
        if (it != g_localPorts.end())
        {
            peer = it->second;
            found = true;
        }
    }
    if (!found) return;

    LogDebug("rewrite from local port %u to %s:%u", localPort,
             IpToString(peer.ip).c_str(), peer.port);
    address->sin_addr.s_addr = peer.ip;
    address->sin_port = htons(peer.port);
    *fromLen = sizeof(sockaddr_in);
}

static bool GetSocketPeer(SOCKET s, PeerKey& peer)
{
    std::lock_guard<std::mutex> lock(g_mapMutex);
    auto it = g_socketPeers.find(s);
    if (it == g_socketPeers.end()) return false;
    peer = it->second;
    return true;
}

static void SetSocketPeer(SOCKET s, PeerKey peer)
{
    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_socketPeers[s] = peer;
}

static void RemoveSocket(SOCKET s)
{
    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_socketPeers.erase(s);
}

// ---------------------------------------------------------------------------
// Injected packet delivery
// ---------------------------------------------------------------------------
static void InjectPayload(uint8_t kind, uint32_t dstVirt, uint16_t srcPort,
                          uint16_t dstPort, const uint8_t* payload, uint16_t length)
{
    if (length == 0 || length > 1400) return;
    SOCKET s = GetInjectSocket(dstVirt, srcPort);
    if (s == INVALID_SOCKET) return;

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(dstPort);
    if (kind == KindBroadcast)
        destination.sin_addr.s_addr = INADDR_BROADCAST;
    else
        destination.sin_addr.s_addr = g_cfg.localIp != 0 ? g_cfg.localIp : htonl(INADDR_LOOPBACK);

    int sent = sendto(s, (const char*)payload, length, 0,
                      (const sockaddr*)&destination, sizeof(destination));
    if (sent == SOCKET_ERROR && kind == KindBroadcast)
    {
        // Some adapters refuse the limited broadcast. Fall back to a unicast
        // on the configured local address; a wildcard-bound game socket still
        // receives it, and the source rewrite below is identical.
        int broadcastError = WSAGetLastError();
        destination.sin_addr.s_addr = g_cfg.localIp != 0 ? g_cfg.localIp : htonl(INADDR_LOOPBACK);
        sent = sendto(s, (const char*)payload, length, 0,
                      (const sockaddr*)&destination, sizeof(destination));
        if (sent == SOCKET_ERROR)
            LogError("inject broadcast failed: %d; unicast fallback failed: %d",
                     broadcastError, WSAGetLastError());
    }
    if (sent == SOCKET_ERROR)
    {
        LogError("inject sendto %s:%u failed: %d", IpToString(destination.sin_addr.s_addr).c_str(),
                 dstPort, WSAGetLastError());
    }
    else
    {
        LogDebug("inject kind=%u from %s:%u to %s:%u len=%u",
                 kind, IpToString(dstVirt).c_str(), srcPort,
                 IpToString(destination.sin_addr.s_addr).c_str(), dstPort, length);
    }
}

static void InjectPacket(const IpcHeader& header, const uint8_t* payload)
{
    InjectPayload(header.kind, header.dstVirt, header.srcPort, header.dstPort,
                  payload, header.len);
}

// ---------------------------------------------------------------------------
// Winsock hook function types
// ---------------------------------------------------------------------------
typedef int (WSAAPI* fn_sendto)(SOCKET, const char*, int, int, const sockaddr*, int);
typedef int (WSAAPI* fn_WSASendTo)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
                                   const sockaddr*, int, LPWSAOVERLAPPED,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI* fn_send)(SOCKET, const char*, int, int);
typedef int (WSAAPI* fn_WSASend)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
                                 LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI* fn_recvfrom)(SOCKET, char*, int, int, sockaddr*, int*);
typedef int (WSAAPI* fn_WSARecvFrom)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                     sockaddr*, LPINT, LPWSAOVERLAPPED,
                                     LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI* fn_recv)(SOCKET, char*, int, int);
typedef int (WSAAPI* fn_WSARecv)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                 LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI* fn_connect)(SOCKET, const sockaddr*, int);
typedef int (WSAAPI* fn_getpeername)(SOCKET, sockaddr*, int*);
typedef int (WSAAPI* fn_closesocket)(SOCKET);
typedef BOOL (WSAAPI* fn_WSAGetOverlappedResult)(SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD);
typedef int (WSAAPI* fn_bind)(SOCKET, const sockaddr*, int);
typedef int (WSAAPI* fn_getsockname)(SOCKET, sockaddr*, int*);
typedef int (WSAAPI* fn_WSAIoctl)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD,
                                  LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI* fn_gethostname)(char*, int);
typedef struct hostent* (WSAAPI* fn_gethostbyname)(const char*);
typedef ULONG (WINAPI* fn_GetAdaptersAddresses)(ULONG, ULONG, PVOID,
                                                PIP_ADAPTER_ADDRESSES, PULONG);
typedef ULONG (WINAPI* fn_GetAdaptersInfo)(PIP_ADAPTER_INFO, PULONG);

static fn_sendto Real_sendto = nullptr;
static fn_WSASendTo Real_WSASendTo = nullptr;
static fn_send Real_send = nullptr;
static fn_WSASend Real_WSASend = nullptr;
static fn_recvfrom Real_recvfrom = nullptr;
static fn_WSARecvFrom Real_WSARecvFrom = nullptr;
static fn_recv Real_recv = nullptr;
static fn_WSARecv Real_WSARecv = nullptr;
static fn_connect Real_connect = nullptr;
static fn_getpeername Real_getpeername = nullptr;
static fn_closesocket Real_closesocket = nullptr;
static fn_WSAGetOverlappedResult Real_WSAGetOverlappedResult = nullptr;
static fn_bind Real_bind = nullptr;
static fn_getsockname Real_getsockname = nullptr;
static fn_WSAIoctl Real_WSAIoctl = nullptr;
static fn_gethostname Real_gethostname = nullptr;
static fn_gethostbyname Real_gethostbyname = nullptr;
static fn_GetAdaptersAddresses Real_GetAdaptersAddresses = nullptr;
static fn_GetAdaptersInfo Real_GetAdaptersInfo = nullptr;

// ---------------------------------------------------------------------------
// Overlapped WSARecvFrom bookkeeping
// ---------------------------------------------------------------------------
struct PendingRecv
{
    SOCKET socket = INVALID_SOCKET;
    sockaddr* from = nullptr;
    int* fromLen = nullptr;
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion = nullptr;
};

static std::mutex g_pendingMutex;
static std::unordered_map<LPWSAOVERLAPPED, PendingRecv> g_pending;

static void CALLBACK CompletionWrapper(DWORD error, DWORD bytes,
                                       LPWSAOVERLAPPED overlapped, DWORD flags)
{
    PendingRecv pending{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        auto it = g_pending.find(overlapped);
        if (it != g_pending.end())
        {
            pending = it->second;
            g_pending.erase(it);
            found = true;
        }
    }
    if (found && error == 0 && bytes > 0)
        RewriteFrom(pending.from, pending.fromLen);
    if (found && pending.completion)
        pending.completion(error, bytes, overlapped, flags);
}

static int GatherBuffers(LPWSABUF buffers, DWORD count, std::vector<char>& out)
{
    size_t total = 0;
    for (DWORD i = 0; i < count; i++)
    {
        if (buffers[i].buf && buffers[i].len > 0)
            total += buffers[i].len;
    }
    if (total == 0 || total > 1400) return -1;
    out.resize(total);
    size_t offset = 0;
    for (DWORD i = 0; i < count; i++)
    {
        if (buffers[i].buf && buffers[i].len > 0)
        {
            memcpy(out.data() + offset, buffers[i].buf, buffers[i].len);
            offset += buffers[i].len;
        }
    }
    return (int)total;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static int WSAAPI Hook_sendto(SOCKET s, const char* buf, int len, int flags,
                              const sockaddr* to, int toLen)
{
    if (buf && len > 0 && to && toLen >= (int)sizeof(sockaddr_in) && to->sa_family == AF_INET)
    {
        const sockaddr_in* address = (const sockaddr_in*)to;
        uint32_t ip = address->sin_addr.s_addr;
        uint16_t port = ntohs(address->sin_port);
        if (port == 9999 && IsBroadcastIp(ip))
        {
            uint16_t sourcePort = GetLocalPort(s);
            if (sourcePort == 0) sourcePort = 9999;
            SendCapture(0, sourcePort, 9999, buf, len, KindBroadcast);
        }
        else if (IsVirtualIp(ip))
        {
            uint16_t sourcePort = GetLocalPort(s);
            SendCapture(ip, sourcePort, port, buf, len, KindData);
            return len;
        }
    }
    return Real_sendto(s, buf, len, flags, to, toLen);
}

static int WSAAPI Hook_WSASendTo(SOCKET s, LPWSABUF buffers, DWORD count,
                                 LPDWORD sent, DWORD flags, const sockaddr* to,
                                 int toLen, LPWSAOVERLAPPED overlapped,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    if (!overlapped && buffers && to && toLen >= (int)sizeof(sockaddr_in) && to->sa_family == AF_INET)
    {
        const sockaddr_in* address = (const sockaddr_in*)to;
        uint32_t ip = address->sin_addr.s_addr;
        uint16_t port = ntohs(address->sin_port);
        if (port == 9999 && IsBroadcastIp(ip))
        {
            std::vector<char> data;
            int length = GatherBuffers(buffers, count, data);
            if (length > 0)
            {
                uint16_t sourcePort = GetLocalPort(s);
                if (sourcePort == 0) sourcePort = 9999;
                SendCapture(0, sourcePort, 9999, data.data(), length, KindBroadcast);
            }
        }
        else if (IsVirtualIp(ip))
        {
            std::vector<char> data;
            int length = GatherBuffers(buffers, count, data);
            if (length > 0)
            {
                SendCapture(ip, GetLocalPort(s), port, data.data(), length, KindData);
                if (sent) *sent = (DWORD)length;
                return 0;
            }
        }
    }
    return Real_WSASendTo(s, buffers, count, sent, flags, to, toLen, overlapped, completion);
}

static int WSAAPI Hook_send(SOCKET s, const char* buf, int len, int flags)
{
    PeerKey peer{};
    if (buf && len > 0 && GetSocketPeer(s, peer))
    {
        SendCapture(peer.ip, GetLocalPort(s), peer.port, buf, len, KindData);
        return len;
    }
    return Real_send(s, buf, len, flags);
}

static int WSAAPI Hook_WSASend(SOCKET s, LPWSABUF buffers, DWORD count,
                               LPDWORD sent, DWORD flags,
                               LPWSAOVERLAPPED overlapped,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    PeerKey peer{};
    if (!overlapped && GetSocketPeer(s, peer))
    {
        std::vector<char> data;
        int length = GatherBuffers(buffers, count, data);
        if (length > 0)
        {
            SendCapture(peer.ip, GetLocalPort(s), peer.port, data.data(), length, KindData);
            if (sent) *sent = (DWORD)length;
            return 0;
        }
    }
    return Real_WSASend(s, buffers, count, sent, flags, overlapped, completion);
}

static int WSAAPI Hook_connect(SOCKET s, const sockaddr* name, int nameLen)
{
    if (name && nameLen >= (int)sizeof(sockaddr_in) && name->sa_family == AF_INET)
    {
        const sockaddr_in* address = (const sockaddr_in*)name;
        uint32_t ip = address->sin_addr.s_addr;
        uint16_t port = ntohs(address->sin_port);
        if (IsVirtualIp(ip))
        {
            SOCKET inject = GetInjectSocket(ip, port);
            if (inject != INVALID_SOCKET)
            {
                sockaddr_in local{};
                int localLen = sizeof(local);
                if (getsockname(inject, (sockaddr*)&local, &localLen) == 0)
                {
                    SetSocketPeer(s, PeerKey{ ip, port });
                    sockaddr_in target{};
                    target.sin_family = AF_INET;
                    target.sin_port = local.sin_port;
                    target.sin_addr.s_addr = g_cfg.localIp != 0 ? g_cfg.localIp : htonl(INADDR_LOOPBACK);
                    LogDebug("redirect connect %s:%u -> local port %u",
                             IpToString(ip).c_str(), port, ntohs(local.sin_port));
                    int result = Real_connect(s, (const sockaddr*)&target, sizeof(target));
                    if (result != 0)
                        RemoveSocket(s);
                    return result;
                }
            }
            LogError("cannot redirect connect to %s:%u", IpToString(ip).c_str(), port);
        }
    }
    return Real_connect(s, name, nameLen);
}

static int WSAAPI Hook_getpeername(SOCKET s, sockaddr* name, int* nameLen)
{
    PeerKey peer{};
    if (GetSocketPeer(s, peer))
    {
        if (name && nameLen && *nameLen >= (int)sizeof(sockaddr_in))
        {
            sockaddr_in* address = (sockaddr_in*)name;
            address->sin_family = AF_INET;
            address->sin_addr.s_addr = peer.ip;
            address->sin_port = htons(peer.port);
            *nameLen = sizeof(sockaddr_in);
            return 0;
        }
        if (nameLen)
        {
            *nameLen = sizeof(sockaddr_in);
            return 0;
        }
    }
    return Real_getpeername(s, name, nameLen);
}

static int WSAAPI Hook_recvfrom(SOCKET s, char* buf, int len, int flags,
                                sockaddr* from, int* fromLen)
{
    int result = Real_recvfrom(s, buf, len, flags, from, fromLen);
    if (result >= 0 && from && fromLen)
        RewriteFrom(from, fromLen);
    return result;
}

static int WSAAPI Hook_WSARecvFrom(SOCKET s, LPWSABUF buffers, DWORD count,
                                   LPDWORD received, LPDWORD flags,
                                   sockaddr* from, LPINT fromLen,
                                   LPWSAOVERLAPPED overlapped,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    if (!overlapped || !from || !fromLen)
    {
        int result = Real_WSARecvFrom(s, buffers, count, received, flags,
                                      from, fromLen, overlapped, completion);
        if (result == 0 && from && fromLen && (!received || *received > 0))
            RewriteFrom(from, fromLen);
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pending[overlapped] = PendingRecv{ s, from, fromLen, completion };
    }

    LPWSAOVERLAPPED_COMPLETION_ROUTINE passCompletion = completion ? CompletionWrapper : nullptr;
    int result = Real_WSARecvFrom(s, buffers, count, received, flags,
                                  from, fromLen, overlapped, passCompletion);

    if (result == 0)
    {
        PendingRecv pending{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            auto it = g_pending.find(overlapped);
            if (it != g_pending.end())
            {
                pending = it->second;
                g_pending.erase(it);
                found = true;
            }
        }
        if (found && (!received || *received > 0))
            RewriteFrom(pending.from, pending.fromLen);
    }
    else
    {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING)
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            g_pending.erase(overlapped);
        }
    }
    return result;
}

static int WSAAPI Hook_recv(SOCKET s, char* buf, int len, int flags)
{
    return Real_recv(s, buf, len, flags);
}

static int WSAAPI Hook_WSARecv(SOCKET s, LPWSABUF buffers, DWORD count,
                               LPDWORD received, LPDWORD flags,
                               LPWSAOVERLAPPED overlapped,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    return Real_WSARecv(s, buffers, count, received, flags, overlapped, completion);
}

static BOOL WSAAPI Hook_WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED overlapped,
                                               LPDWORD transferred, BOOL wait,
                                               LPDWORD flags)
{
    BOOL result = Real_WSAGetOverlappedResult(s, overlapped, transferred, wait, flags);
    if (result)
    {
        PendingRecv pending{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            auto it = g_pending.find(overlapped);
            if (it != g_pending.end())
            {
                pending = it->second;
                g_pending.erase(it);
                found = true;
            }
        }
        if (found && transferred && *transferred > 0)
            RewriteFrom(pending.from, pending.fromLen);
    }
    return result;
}

static int WSAAPI Hook_closesocket(SOCKET s)
{
    RemoveSocket(s);
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        for (auto it = g_pending.begin(); it != g_pending.end();)
        {
            if (it->second.socket == s)
                it = g_pending.erase(it);
            else
                ++it;
        }
    }
    return Real_closesocket(s);
}

// ---------------------------------------------------------------------------
// "Who am I" hooks: make the game believe its own address is the relay's
// virtual address, so both peers look like they are on the same LAN.
// ---------------------------------------------------------------------------
static char g_localHostName[256] = {};

static int WSAAPI Hook_gethostname(char* name, int length)
{
    int result = Real_gethostname(name, length);
    if (result == 0 && name)
    {
        strncpy(g_localHostName, name, sizeof(g_localHostName) - 1);
        g_localHostName[sizeof(g_localHostName) - 1] = '\0';
    }
    LogDebug("gethostname -> '%s' result=%d", result == 0 && name ? name : "?", result);
    return result;
}

static struct hostent g_fakeHostent{};
static in_addr g_fakeHostAddress{};
static char* g_fakeAliases[1] = { nullptr };
static char* g_fakeAddressList[2] = { nullptr, nullptr };
static char g_fakeHostName[] = "net-goal-11";

static bool IsPrivateIpv4(uint32_t ip)
{
    uint8_t a = (uint8_t)(ip & 0xFF);
    uint8_t b = (uint8_t)((ip >> 8) & 0xFF);
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168);
}

static struct hostent* ReturnFakeHost()
{
    g_fakeHostAddress.s_addr = g_cfg.selfVirt;
    g_fakeAddressList[0] = (char*)&g_fakeHostAddress;
    g_fakeAddressList[1] = nullptr;
    g_fakeHostent.h_name = g_fakeHostName;
    g_fakeHostent.h_aliases = g_fakeAliases;
    g_fakeHostent.h_addrtype = AF_INET;
    g_fakeHostent.h_length = 4;
    g_fakeHostent.h_addr_list = g_fakeAddressList;
    return &g_fakeHostent;
}

static struct hostent* WSAAPI Hook_gethostbyname(const char* name)
{
    if (g_cfg.selfVirt != 0 && name &&
        (_stricmp(name, "localhost") == 0 || _stricmp(name, "net-goal-11") == 0 ||
         (g_localHostName[0] && _stricmp(name, g_localHostName) == 0)))
    {
        LogDebug("gethostbyname(%s) -> %s (fake)", name, IpToString(g_cfg.selfVirt).c_str());
        return ReturnFakeHost();
    }
    struct hostent* real = Real_gethostbyname(name);
    if (real && real->h_addr_list && real->h_addr_list[0] && real->h_addrtype == AF_INET)
    {
        uint32_t address = *(uint32_t*)real->h_addr_list[0];
        LogDebug("gethostbyname(%s) -> %s (real)", name ? name : "?",
                 IpToString(address).c_str());
        if (g_cfg.selfVirt != 0 && (address == 0 || address == g_cfg.localIp ||
                                    IsPrivateIpv4(address)))
        {
            LogInfo("gethostbyname(%s) returned local %s -> %s", name ? name : "?",
                    IpToString(address).c_str(), IpToString(g_cfg.selfVirt).c_str());
            return ReturnFakeHost();
        }
    }
    else
    {
        LogDebug("gethostbyname(%s) -> none", name ? name : "?");
    }
    return real;
}

static int WSAAPI Hook_bind(SOCKET socket, const sockaddr* name, int nameLength)
{
    if (g_cfg.selfVirt != 0 && name && nameLength >= (int)sizeof(sockaddr_in) &&
        name->sa_family == AF_INET)
    {
        const sockaddr_in* address = (const sockaddr_in*)name;
        if (address->sin_addr.s_addr == g_cfg.selfVirt)
        {
            sockaddr_in wildcard = *address;
            wildcard.sin_addr.s_addr = INADDR_ANY;
            int result = Real_bind(socket, (const sockaddr*)&wildcard, (int)sizeof(wildcard));
            LogInfo("bind %s:%u -> 0.0.0.0:%u result=%d",
                    IpToString(address->sin_addr.s_addr).c_str(), ntohs(address->sin_port),
                    ntohs(wildcard.sin_port), result);
            return result;
        }
        int result = Real_bind(socket, name, nameLength);
        LogDebug("bind %s:%u result=%d", IpToString(address->sin_addr.s_addr).c_str(),
                 ntohs(address->sin_port), result);
        return result;
    }
    return Real_bind(socket, name, nameLength);
}

static int WSAAPI Hook_getsockname(SOCKET socket, sockaddr* name, int* nameLength)
{
    int result = Real_getsockname(socket, name, nameLength);
    if (result == 0 && g_cfg.selfVirt != 0 && name && nameLength &&
        *nameLength >= (int)sizeof(sockaddr_in) && name->sa_family == AF_INET)
    {
        sockaddr_in* address = (sockaddr_in*)name;
        uint32_t original = address->sin_addr.s_addr;
        if (original != htonl(INADDR_LOOPBACK))
        {
            address->sin_addr.s_addr = g_cfg.selfVirt;
            LogDebug("getsockname %s:%u -> %s", IpToString(original).c_str(),
                     ntohs(address->sin_port), IpToString(g_cfg.selfVirt).c_str());
        }
    }
    return result;
}

static int WSAAPI Hook_WSAIoctl(SOCKET socket, DWORD code, LPVOID inBuffer, DWORD inSize,
                                LPVOID outBuffer, DWORD outSize, LPDWORD returned,
                                LPWSAOVERLAPPED overlapped,
                                LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    int result = Real_WSAIoctl(socket, code, inBuffer, inSize, outBuffer, outSize,
                               returned, overlapped, completion);
    LogDebug("WSAIoctl code=0x%08lx in=%lu out=%lu -> %d outBytes=%lu",
             code, inSize, outSize, result, returned ? *returned : 0);
    if (result == 0 && g_cfg.selfVirt != 0 && code == SIO_ADDRESS_LIST_QUERY &&
        outBuffer && returned && *returned >= sizeof(SOCKET_ADDRESS_LIST))
    {
        SpoofAddressList((PSOCKET_ADDRESS_LIST)outBuffer);
    }
#ifdef SIO_GET_INTERFACE_LIST
    if (result == 0 && g_cfg.selfVirt != 0 && code == SIO_GET_INTERFACE_LIST &&
        outBuffer && returned)
    {
        DWORD count = *returned / sizeof(INTERFACE_INFO);
        for (DWORD i = 0; i < count; ++i)
        {
            INTERFACE_INFO* info = (INTERFACE_INFO*)outBuffer + i;
            sockaddr_in* address = (sockaddr_in*)&info->iiAddress;
            if (address->sin_family == AF_INET)
                address->sin_addr.s_addr = g_cfg.selfVirt;
        }
        LogInfo("interface list spoof: %lu entries -> %s", count,
                IpToString(g_cfg.selfVirt).c_str());
    }
#endif
    // The game asks Windows "which local address would you use to reach this
    // destination?" via SIO_ROUTING_INTERFACE_QUERY.  Without this rewrite it
    // gets the real 192.168.x.x and puts that address into the LAN payload,
    // so the peer tries to connect to an unreachable address and times out.
    if (result == 0 && g_cfg.selfVirt != 0 && code == SIO_ROUTING_INTERFACE_QUERY &&
        outBuffer && returned && *returned >= sizeof(sockaddr_in))
    {
        sockaddr_in* route = (sockaddr_in*)outBuffer;
        if (route->sin_family == AF_INET)
        {
            uint32_t original = route->sin_addr.s_addr;
            route->sin_addr.s_addr = g_cfg.selfVirt;
            LogInfo("routing interface spoof: %s -> %s",
                    IpToString(original).c_str(), IpToString(g_cfg.selfVirt).c_str());
        }
    }
    return result;
}

static ULONG WINAPI Hook_GetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved,
                                              PIP_ADAPTER_ADDRESSES addresses, PULONG size)
{
    ULONG result = Real_GetAdaptersAddresses(family, flags, reserved, addresses, size);
    LogInfo("GetAdaptersAddresses family=%lu flags=0x%lx -> %lu size=%lu self=%s",
            family, flags, result, size ? *size : 0, IpToString(g_cfg.selfVirt).c_str());
    if (result == NO_ERROR && addresses && g_cfg.selfVirt != 0)
        SpoofAdapterAddresses(addresses);
    return result;
}

static ULONG WINAPI Hook_GetAdaptersInfo(PIP_ADAPTER_INFO info, PULONG size)
{
    ULONG result = Real_GetAdaptersInfo(info, size);
    LogInfo("GetAdaptersInfo -> %lu size=%lu", result, size ? *size : 0);
    if (result == NO_ERROR && info && g_cfg.selfVirt != 0)
        SpoofAdapterInfo(info);
    return result;
}

// ---------------------------------------------------------------------------
// IAT patching
// ---------------------------------------------------------------------------
static bool PatchIat(HMODULE module, const char* dllName, const char* functionName,
                     WORD ordinal, void* hook, void** original)
{
    if (!module) return false;
    auto* dos = (IMAGE_DOS_HEADER*)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS*)((BYTE*)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    DWORD importRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (importRva == 0) return false;
    auto* descriptor = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)module + importRva);
    bool patched = false;

    for (; descriptor->Name != 0; descriptor++)
    {
        const char* moduleName = (const char*)((BYTE*)module + descriptor->Name);
        if (_stricmp(moduleName, dllName) != 0) continue;
        if (descriptor->OriginalFirstThunk == 0) continue;

        auto* originalThunk = (IMAGE_THUNK_DATA*)((BYTE*)module + descriptor->OriginalFirstThunk);
        auto* firstThunk = (IMAGE_THUNK_DATA*)((BYTE*)module + descriptor->FirstThunk);
        for (; originalThunk->u1.AddressOfData != 0; originalThunk++, firstThunk++)
        {
            bool match = false;
            if (ordinal != 0)
            {
                if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal))
                    match = IMAGE_ORDINAL(originalThunk->u1.Ordinal) == ordinal;
            }
            else if (functionName && !IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal))
            {
                auto* importByName = (IMAGE_IMPORT_BY_NAME*)((BYTE*)module + originalThunk->u1.AddressOfData);
                match = strcmp((const char*)importByName->Name, functionName) == 0;
            }

            if (!match) continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(&firstThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect))
                continue;
            if (original) *original = (void*)firstThunk->u1.Function;
            firstThunk->u1.Function = (ULONG_PTR)hook;
            DWORD ignored = 0;
            VirtualProtect(&firstThunk->u1.Function, sizeof(void*), oldProtect, &ignored);
            patched = true;
            LogInfo("patched %s!%s", dllName, functionName ? functionName : "ordinal");
        }
    }
    return patched;
}

static void InstallHooks()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    int count = 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 20, (void*)Hook_sendto, (void**)&Real_sendto) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 17, (void*)Hook_recvfrom, (void**)&Real_recvfrom) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 19, (void*)Hook_send, (void**)&Real_send) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 16, (void*)Hook_recv, (void**)&Real_recv) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 4, (void*)Hook_connect, (void**)&Real_connect) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 5, (void*)Hook_getpeername, (void**)&Real_getpeername) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 3, (void*)Hook_closesocket, (void**)&Real_closesocket) ? 1 : 0;

    count += PatchIat(exe, "WS2_32.dll", "WSARecvFrom", 0, (void*)Hook_WSARecvFrom, (void**)&Real_WSARecvFrom) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "WSARecv", 0, (void*)Hook_WSARecv, (void**)&Real_WSARecv) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "WSASend", 0, (void*)Hook_WSASend, (void**)&Real_WSASend) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "WSASendTo", 0, (void*)Hook_WSASendTo, (void**)&Real_WSASendTo) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "WSAGetOverlappedResult", 0, (void*)Hook_WSAGetOverlappedResult, (void**)&Real_WSAGetOverlappedResult) ? 1 : 0;

    // Also cover binaries that import the classic functions from WS2_32 by
    // name instead of WSOCK32 by ordinal (the game itself uses the latter).
    count += PatchIat(exe, "WS2_32.dll", "sendto", 0, (void*)Hook_sendto, (void**)&Real_sendto) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "recvfrom", 0, (void*)Hook_recvfrom, (void**)&Real_recvfrom) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "send", 0, (void*)Hook_send, (void**)&Real_send) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "recv", 0, (void*)Hook_recv, (void**)&Real_recv) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "connect", 0, (void*)Hook_connect, (void**)&Real_connect) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "getpeername", 0, (void*)Hook_getpeername, (void**)&Real_getpeername) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "closesocket", 0, (void*)Hook_closesocket, (void**)&Real_closesocket) ? 1 : 0;

    // Some linkers import the classic WS2_32 entry points by ordinal rather
    // than by name (the synthetic test does). Cover both forms.
    count += PatchIat(exe, "WS2_32.dll", nullptr, 3, (void*)Hook_closesocket, (void**)&Real_closesocket) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", nullptr, 4, (void*)Hook_connect, (void**)&Real_connect) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", nullptr, 5, (void*)Hook_getpeername, (void**)&Real_getpeername) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", nullptr, 16, (void*)Hook_recv, (void**)&Real_recv) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", nullptr, 17, (void*)Hook_recvfrom, (void**)&Real_recvfrom) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", nullptr, 19, (void*)Hook_send, (void**)&Real_send) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", nullptr, 20, (void*)Hook_sendto, (void**)&Real_sendto) ? 1 : 0;

    // "Who am I" surface: adapter enumeration, local-name resolution and
    // local address queries.  Without these the game sees the physical NIC address
    // and treats the relay peer (10.213.77.x) as a different network.
    count += PatchIat(exe, "WS2_32.dll", "bind", 0, (void*)Hook_bind, (void**)&Real_bind) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "getsockname", 0, (void*)Hook_getsockname, (void**)&Real_getsockname) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "WSAIoctl", 0, (void*)Hook_WSAIoctl, (void**)&Real_WSAIoctl) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "gethostname", 0, (void*)Hook_gethostname, (void**)&Real_gethostname) ? 1 : 0;
    count += PatchIat(exe, "WS2_32.dll", "gethostbyname", 0, (void*)Hook_gethostbyname, (void**)&Real_gethostbyname) ? 1 : 0;
    count += PatchIat(exe, "IPHLPAPI.DLL", "GetAdaptersAddresses", 0, (void*)Hook_GetAdaptersAddresses, (void**)&Real_GetAdaptersAddresses) ? 1 : 0;
    count += PatchIat(exe, "IPHLPAPI.DLL", "GetAdaptersInfo", 0, (void*)Hook_GetAdaptersInfo, (void**)&Real_GetAdaptersInfo) ? 1 : 0;

    // Winsock 1.1 ordinal imports used by the game executable.
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 2, (void*)Hook_bind, (void**)&Real_bind) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 6, (void*)Hook_getsockname, (void**)&Real_getsockname) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 52, (void*)Hook_gethostbyname, (void**)&Real_gethostbyname) ? 1 : 0;
    count += PatchIat(exe, "WSOCK32.dll", nullptr, 57, (void*)Hook_gethostname, (void**)&Real_gethostname) ? 1 : 0;

    g_hooksInstalled = count > 0;
    LogInfo("hook installation finished: %d functions patched", count);
    if (!g_hooksInstalled)
        LogError("no Winsock imports patched - is this the game process?");
}

// ---------------------------------------------------------------------------
// IPC receive thread
// ---------------------------------------------------------------------------
static DWORD WINAPI IpcReceiveThread(LPVOID)
{
    uint8_t buffer[2048];
    DWORD lastReady = GetTickCount();
    while (g_running)
    {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int received = recvfrom(g_ipcSocket, (char*)buffer, sizeof(buffer), 0,
                                (sockaddr*)&from, &fromLen);
        if (received == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK)
            {
                // Re-announce periodically so a restarted GUI can discover a
                // game that was injected by a previous client process.
                DWORD now = GetTickCount();
                if (now - lastReady >= 3000)
                {
                    lastReady = now;
                    SendReady();
                }
                continue;
            }
            if (error == WSAENOTSOCK) break;
            if (error == WSAECONNRESET) continue;
            Sleep(20);
            continue;
        }
        if (received < (int)sizeof(IpcHeader)) continue;
        auto* header = (IpcHeader*)buffer;
        if (header->magic != IpcMagic) continue;
        if (header->len > received - (int)sizeof(IpcHeader)) continue;

        switch (header->type)
        {
        case IpcInject:
            InjectPacket(*header, buffer + sizeof(IpcHeader));
            break;
        case IpcConfig:
            if (header->len >= 12)
            {
                const uint8_t* data = buffer + sizeof(IpcHeader);
                uint32_t subnet, mask, localIp;
                memcpy(&subnet, data, 4);
                memcpy(&mask, data + 4, 4);
                memcpy(&localIp, data + 8, 4);
                g_cfg.subnet = subnet & mask;
                g_cfg.mask = mask;
                g_cfg.localIp = localIp;
                if (header->len >= 16)
                {
                    uint32_t selfVirt;
                    memcpy(&selfVirt, data + 12, 4);
                    if (selfVirt != 0 && selfVirt != g_cfg.selfVirt)
                    {
                        g_cfg.selfVirt = selfVirt;
                        LogInfo("virtual self address updated: %s (restart the game if it already queried adapters)",
                                IpToString(selfVirt).c_str());
                    }
                }
                LogInfo("config updated: subnet=%s mask=%s local=%s self=%s",
                        IpToString(g_cfg.subnet).c_str(), IpToString(g_cfg.mask).c_str(),
                        IpToString(g_cfg.localIp).c_str(), IpToString(g_cfg.selfVirt).c_str());
            }
            break;
        case IpcRelayConfig:
            if (header->len >= sizeof(fr::IpcRelayConfigPayload))
            {
                fr::IpcRelayConfigPayload cfg{};
                memcpy(&cfg, buffer + sizeof(IpcHeader), sizeof(cfg));
                StopRelay();
                g_cfg.direct = true;
                g_cfg.serverPort = (int)cfg.serverPort;
                g_cfg.passwordHash = cfg.passwordHash;
                size_t serverLen = strnlen(cfg.server, sizeof(cfg.server));
                g_cfg.serverHost.assign(cfg.server, serverLen);
                size_t roomLen = strnlen(cfg.room, sizeof(cfg.room));
                g_cfg.room.assign(cfg.room, roomLen);
                size_t nameLen = strnlen(cfg.name, sizeof(cfg.name));
                g_cfg.name.assign(cfg.name, nameLen);
                if (g_cfg.serverHost.empty()) g_cfg.serverHost = "127.0.0.1";
                if (g_cfg.room.empty()) g_cfg.room = "OL";
                if (g_cfg.name.empty()) g_cfg.name = "Player";
                LogInfo("relay config from GUI: server=%s:%d room=%s name=%s",
                        g_cfg.serverHost.c_str(), g_cfg.serverPort,
                        g_cfg.room.c_str(), g_cfg.name.c_str());
                StartRelay();
            }
            break;
        case IpcRelayStop:
            StopRelay();
            SendRelayStatus(fr::RelayDisconnected, "Relay disconnected");
            break;
        case IpcChat:
            if (header->len > 0)
            {
                std::string text((const char*)buffer + sizeof(IpcHeader), header->len);
                if (!RelaySendChat(text))
                {
                    uint8_t reply[fr::kMaxPayload];
                    size_t replyLength = fr::EncodeChat(reply, "System", "Not connected to the relay; chat not sent");
                    SendIpc(IpcChat, 0, 0, 0, 0, reply, (uint16_t)replyLength);
                }
            }
            break;
        default:
            break;
        }
    }
    return 0;
}

static void IpcInit()
{
    g_ipcSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_ipcSocket == INVALID_SOCKET)
    {
        LogError("ipc socket() failed: %d", WSAGetLastError());
        return;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = inet_addr("127.0.0.1");
    local.sin_port = htons((u_short)g_cfg.dllPort);
    if (bind(g_ipcSocket, (sockaddr*)&local, sizeof(local)) != 0)
    {
        LogError("ipc bind 127.0.0.1:%d failed: %d", g_cfg.dllPort, WSAGetLastError());
        closesocket(g_ipcSocket);
        g_ipcSocket = INVALID_SOCKET;
        return;
    }

    g_guiAddress.sin_family = AF_INET;
    g_guiAddress.sin_addr.s_addr = inet_addr("127.0.0.1");
    g_guiAddress.sin_port = htons((u_short)g_cfg.guiPort);

    // Control socket only: block with a timeout instead of polling with a
    // 5 ms sleep.  Direct game traffic bypasses this socket entirely.
    DWORD timeoutMs = 500;
    setsockopt(g_ipcSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    BOOL noReset = FALSE;
    DWORD ignored = 0;
    WSAIoctl(g_ipcSocket, SIO_UDP_CONNRESET, &noReset, sizeof(noReset),
             nullptr, 0, &ignored, nullptr, nullptr);

    HANDLE thread = CreateThread(nullptr, 0, IpcReceiveThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    LogInfo("ipc listening on 127.0.0.1:%d -> gui 127.0.0.1:%d", g_cfg.dllPort, g_cfg.guiPort);
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
static void LoadConfig()
{
    g_moduleDir = GetModuleDirectory(g_hModule);
    g_iniPath = g_moduleDir + L"\\Net Goal 11.Hook.ini";

    g_cfg.guiPort = IniInt(L"gui_port", 45889);
    g_cfg.dllPort = IniInt(L"dll_port", 45890);
    g_cfg.logLevel = IniInt(L"log_level", 1);

    g_cfg.direct = IniInt(L"direct", 0) != 0;
    g_cfg.serverHost = WideToUtf8(IniString(L"server", L"127.0.0.1"));
    g_cfg.serverPort = IniInt(L"server_port", fr::kDefaultPort);
    g_cfg.room = WideToUtf8(IniString(L"room", L"OL"));
    g_cfg.name = WideToUtf8(IniString(L"name", L"Player"));
    g_cfg.passwordHash = wcstoull(IniString(L"password_hash", L"0").c_str(), nullptr, 0);
    if (g_cfg.serverHost.empty()) g_cfg.serverHost = "127.0.0.1";
    if (g_cfg.serverPort < 1 || g_cfg.serverPort > 65535) g_cfg.serverPort = fr::kDefaultPort;
    if (g_cfg.room.empty()) g_cfg.room = "OL";
    if (g_cfg.name.empty()) g_cfg.name = "Player";

    g_cfg.direct = IniInt(L"direct", 0) != 0;
    g_cfg.serverHost = WideToUtf8(IniString(L"server", L"127.0.0.1"));
    g_cfg.serverPort = IniInt(L"server_port", fr::kDefaultPort);
    g_cfg.room = WideToUtf8(IniString(L"room", L"OL"));
    g_cfg.name = WideToUtf8(IniString(L"name", L"Player"));
    g_cfg.passwordHash = wcstoull(IniString(L"password_hash", L"0").c_str(), nullptr, 0);
    if (g_cfg.serverHost.empty()) g_cfg.serverHost = "127.0.0.1";
    if (g_cfg.serverPort < 1 || g_cfg.serverPort > 65535) g_cfg.serverPort = fr::kDefaultPort;
    if (g_cfg.room.empty()) g_cfg.room = "OL";
    if (g_cfg.name.empty()) g_cfg.name = "Player";

    std::wstring subnetText = IniString(L"virtual_subnet", L"10.213.77.0");
    std::wstring maskText = IniString(L"virtual_mask", L"255.255.255.0");
    std::wstring localText = IniString(L"local_ip", L"127.0.0.1");
    std::wstring selfText = IniString(L"virtual_ip", L"0");
    std::wstring logText = IniString(L"log_path", L"");

    g_cfg.subnet = IpFromString(WideToAnsi(subnetText).c_str());
    g_cfg.mask = IpFromString(WideToAnsi(maskText).c_str());
    g_cfg.localIp = IpFromString(WideToAnsi(localText).c_str());
    g_cfg.selfVirt = IpFromString(WideToAnsi(selfText).c_str());
    if (g_cfg.mask == 0) g_cfg.mask = IpFromString("255.255.255.0");
    g_cfg.subnet &= g_cfg.mask;
    g_cfg.logPath = logText;
}

static DWORD WINAPI WorkerThread(LPVOID)
{
    LogInit();
    LogInfo("Net Goal 11.Hook loaded pid=%lu module=%p", GetCurrentProcessId(), g_hModule);
    LogInfo("config subnet=%s mask=%s local=%s self=%s gui=%d dll=%d",
            IpToString(g_cfg.subnet).c_str(), IpToString(g_cfg.mask).c_str(),
            IpToString(g_cfg.localIp).c_str(), IpToString(g_cfg.selfVirt).c_str(),
            g_cfg.guiPort, g_cfg.dllPort);
    LogInfo("hooks installed=%d", g_hooksInstalled ? 1 : 0);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        LogError("WSAStartup failed");
        return 0;
    }
    g_wsaStarted = true;

    IpcInit();

    SendReady();
    if (g_cfg.direct)
        StartRelay();
    return 0;
}
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = module;
        DisableThreadLibraryCalls(module);
        // Install the hooks before the loader resumes the game's main thread,
        // otherwise the game can enumerate adapters before the virtual address is
        // known and cache the physical NIC address.
        LoadConfig();
        InstallHooks();
        HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running = false;
        StopRelay();
        if (g_ipcSocket != INVALID_SOCKET)
        {
            closesocket(g_ipcSocket);
            g_ipcSocket = INVALID_SOCKET;
        }
        if (g_logFile)
        {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }
    return TRUE;
}
