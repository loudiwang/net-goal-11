// Synthetic 32-bit target for the direct Hook relay.
// It loads Net Goal 11.Hook.dll, sends a virtual-LAN datagram to a peer and
// waits for the relayed reply.  Run it after a peer has joined the same room.
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

static void WriteIni(const wchar_t* path, const wchar_t* logPath)
{
    FILE* file = nullptr;
    _wfopen_s(&file, path, L"w");
    if (!file) return;
    std::fputs("[hook]\n", file);
    std::fputs("gui_port=45989\n", file);
    std::fputs("dll_port=45990\n", file);
    std::fputs("direct=1\n", file);
    std::fputs("server=127.0.0.1\n", file);
    std::fputs("server_port=45999\n", file);
    std::fputs("room=DIRECT\n", file);
    std::fputs("password_hash=0\n", file);
    std::fputs("name=Alice\n", file);
    std::fputs("virtual_subnet=10.213.77.0\n", file);
    std::fputs("virtual_mask=255.255.255.0\n", file);
    std::fputs("virtual_ip=0\n", file);
    std::fputs("local_ip=0.0.0.0\n", file);
    std::fputs("log_level=2\n", file);
    std::fprintf(file, "log_path=%ls\n", logPath);
    std::fclose(file);
}

int main()
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("WSAStartup failed\n");
        return 2;
    }

    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    wchar_t directory[MAX_PATH] = {};
    wcscpy_s(directory, modulePath);
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (slash) *slash = L'\0';

    wchar_t dllPath[MAX_PATH] = {};
    wchar_t iniPath[MAX_PATH] = {};
    wchar_t logPath[MAX_PATH] = {};
    swprintf_s(dllPath, L"%s\\Net Goal 11.Hook.dll", directory);
    swprintf_s(iniPath, L"%s\\Net Goal 11.Hook.ini", directory);
    swprintf_s(logPath, L"%s\\hook.log", directory);
    WriteIni(iniPath, logPath);
    DeleteFileW(logPath);

    HMODULE hook = LoadLibraryW(dllPath);
    if (!hook)
    {
        std::printf("LoadLibraryW failed: %lu\n", GetLastError());
        WSACleanup();
        return 3;
    }
    std::printf("hook=%p, waiting for direct relay\n", hook);
    Sleep(2500);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        std::printf("socket failed: %d\n", WSAGetLastError());
        return 4;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(3306);
    if (bind(s, (sockaddr*)&local, sizeof(local)) != 0)
    {
        std::printf("bind 3306 failed: %d\n", WSAGetLastError());
        return 5;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = inet_addr("10.213.77.2");
    peer.sin_port = htons(3306);
    const char* message = "HOOK->BOB";
    int sent = sendto(s, message, (int)strlen(message), 0, (sockaddr*)&peer, sizeof(peer));
    std::printf("sendto virtual peer: %d (%d)\n", sent, WSAGetLastError());

    DWORD timeout = 8000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    char buffer[2048] = {};
    sockaddr_in from{};
    int fromLength = sizeof(from);
    int received = recvfrom(s, buffer, sizeof(buffer), 0, (sockaddr*)&from, &fromLength);
    if (received > 0)
    {
        std::printf("recv %d from %s:%u payload=%.*s\n", received,
                    inet_ntoa(from.sin_addr), ntohs(from.sin_port), received, buffer);
    }
    else
    {
        std::printf("recv failed: %d\n", WSAGetLastError());
    }

    closesocket(s);

    // --- broadcast discovery path -----------------------------------------
    SOCKET beacon = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL reuse = TRUE;
    BOOL broadcastEnable = TRUE;
    setsockopt(beacon, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    setsockopt(beacon, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcastEnable, sizeof(broadcastEnable));
    sockaddr_in beaconLocal{};
    beaconLocal.sin_family = AF_INET;
    beaconLocal.sin_addr.s_addr = INADDR_ANY;
    beaconLocal.sin_port = htons(9999);
    if (bind(beacon, (sockaddr*)&beaconLocal, sizeof(beaconLocal)) != 0)
    {
        std::printf("bind 9999 failed: %d\n", WSAGetLastError());
        closesocket(beacon);
        FreeLibrary(hook);
        WSACleanup();
        return 6;
    }
    DWORD beaconTimeout = 8000;
    setsockopt(beacon, SOL_SOCKET, SO_RCVTIMEO, (const char*)&beaconTimeout, sizeof(beaconTimeout));

    sockaddr_in broadcastAddress{};
    broadcastAddress.sin_family = AF_INET;
    broadcastAddress.sin_addr.s_addr = INADDR_BROADCAST;
    broadcastAddress.sin_port = htons(9999);
    const char* beaconMessage = "BEACON";
    std::printf("sendto broadcast: %d\n",
                sendto(beacon, beaconMessage, (int)strlen(beaconMessage), 0,
                       (sockaddr*)&broadcastAddress, sizeof(broadcastAddress)));

    bool beaconOk = false;
    DWORD beaconDeadline = GetTickCount() + 8000;
    while (GetTickCount() < beaconDeadline)
    {
        sockaddr_in beaconFrom{};
        int beaconFromLength = sizeof(beaconFrom);
        int beaconReceived = recvfrom(beacon, buffer, sizeof(buffer), 0,
                                      (sockaddr*)&beaconFrom, &beaconFromLength);
        if (beaconReceived <= 0) continue;
        if (beaconReceived == 12 && std::memcmp(buffer, "BEACON-REPLY", 12) == 0)
        {
            beaconOk = true;
            std::printf("broadcast recv from %s:%u payload=%.*s\n",
                        inet_ntoa(beaconFrom.sin_addr), ntohs(beaconFrom.sin_port),
                        beaconReceived, buffer);
            break;
        }
        std::printf("broadcast ignored local/other packet len=%d\n", beaconReceived);
    }
    closesocket(beacon);

    FreeLibrary(hook);
    WSACleanup();
    return (received > 0 && beaconOk) ? 0 : 1;
}
