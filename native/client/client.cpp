// Net Goal 11 native Win32 client (x86).
//
// The GUI only configures/injects the hook.  Game traffic goes directly from
// Net Goal 11.Hook.dll to the VPS, eliminating the C# client's two loopback UDP
// hops.  The localhost IPC socket is used only for status, peer list, counters
// and reconnect/disconnect commands.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <mstcpip.h>
#include <propidl.h>
#include <gdiplus.h>

#include "../common/protocol.h"
#include "../common/pinned_keys.h"
#include "../common/secure.h"
#include "../common/game_names.h"
#include "../common/version.h"
#include "login.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr int kClientWidth = 1046;
constexpr int kClientHeight = 735;
constexpr int kTitleBarHeight = 48;
constexpr int kHeroTop = 48;
constexpr int kHeroBottom = 158;
constexpr int kRoomListLeft = 274;
constexpr int kRoomListRight = 770;
constexpr int kRoomListTop = 232;
constexpr int kRoomRowHeight = 72;
constexpr int kRoomListVisible = 6;
constexpr wchar_t kLogScrollClass[] = L"NetGoal11ThinScroll";
constexpr int kLogScrollWidth = 8;
constexpr int kLogScrollInset = 3;
constexpr UINT WM_APP_HOOK_EVENT = WM_APP + 1;
constexpr UINT WM_APP_LOBBY_EVENT = WM_APP + 2;
constexpr UINT_PTR IDT_LOBBY_TIMER = 1;

enum ControlId {
    ID_SERVER = 100,
    ID_PORT,
    ID_ROOM,
    ID_PASSWORD,
    ID_NAME,
    ID_GAME,
    ID_BROWSE,
    ID_CONNECT,
    ID_SAVE_CONFIG,
    ID_JOIN_ROOM,
    ID_LEAVE_ROOM,
    ID_MODE_11,
    ID_MODE_22,
    ID_MODE_33,
    ID_MODE_44,
    ID_LAUNCH,
    ID_ATTACH,
    ID_LOCAL_GAME,
    ID_DISCONNECT,
    ID_STATUS,
    ID_PEERS,
    ID_LOG,
    ID_STATS,
    ID_VERBOSE,
    ID_CHAT_LOG,
    ID_CHAT_INPUT,
    ID_CHAT_SEND,
    ID_TAB_LOBBY,
    ID_TAB_ROOM,
    ID_HELP,
    ID_SETTINGS,
    ID_ABOUT,
    ID_EXIT_APP,
    ID_NEW_ROOM,
    ID_REFRESH_ROOMS,
    ID_START_MATCH,
    ID_ROOM_LEAVE,
    ID_DLG_OK,
    ID_DLG_CANCEL,
    ID_DLG_MODE_11,
    ID_DLG_MODE_22,
    ID_DLG_MODE_33,
    ID_DLG_MODE_44,
    ID_DLG_MODE_55,
    ID_ROOM_CHAT_LOG,
    ID_ROOM_CHAT_INPUT,
    ID_ROOM_CHAT_SEND,
};

struct HookEvent {
    uint8_t type = 0;
    std::string payload;
};

struct LobbyEvent {
    uint8_t type = 0;
    std::string payload;
};

struct LobbyMemberInfo {
    std::wstring name;
    bool host = false;
    bool hasGame = false;
};

struct LobbyRoomInfo {
    std::wstring code;
    std::wstring host;
    int mode = 1;
    int members = 0;
    int capacity = 2;
    bool started = false;
    bool hasPassword = false;
};

HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
HWND g_server = nullptr;
HWND g_port = nullptr;
HWND g_room = nullptr;
HWND g_password = nullptr;
HWND g_name = nullptr;
HWND g_game = nullptr;
HWND g_status = nullptr;
HWND g_peers = nullptr;
HWND g_log = nullptr;
HWND g_stats = nullptr;
HWND g_verbose = nullptr;
HWND g_localGame = nullptr;
HWND g_logScroll = nullptr;
bool g_scrollDragging = false;
bool g_scrollHover = false;
int g_scrollDragStartY = 0;
int g_scrollDragStartPos = 0;
int g_scrollDragTrackPixels = 1;
int g_scrollDragLineRange = 1;
HWND g_chatLog = nullptr;
HWND g_chatInput = nullptr;
HWND g_chatSend = nullptr;
HWND g_tabLobby = nullptr;
HWND g_tabRoom = nullptr;
HWND g_tree = nullptr;
HWND g_browse = nullptr;
HWND g_connect = nullptr;
HWND g_saveConfig = nullptr;
HWND g_join = nullptr;
HWND g_leaveRoom = nullptr;
HWND g_modeButtons[4] = {};
HWND g_roomInfo = nullptr;
HWND g_launch = nullptr;
HWND g_attach = nullptr;
HWND g_disconnect = nullptr;
HWND g_help = nullptr;
HWND g_settings = nullptr;
HWND g_about = nullptr;
HWND g_exitApp = nullptr;
HWND g_newRoom = nullptr;
HWND g_refreshRooms = nullptr;
HWND g_roomWindow = nullptr;
HTREEITEM g_treeRoomItem = nullptr;
HTREEITEM g_treeLobbyItem = nullptr;
int g_activeTab = 0;

HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_bannerFont = nullptr;
HFONT g_smallFont = nullptr;
HFONT g_inputFont = nullptr;
HFONT g_chatFont = nullptr;
HFONT g_roomButtonFont = nullptr;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_panelBrush = nullptr;
HBRUSH g_editBrush = nullptr;
HBRUSH g_whiteBrush = nullptr;
Gdiplus::Image* g_heroImage = nullptr;
ULONG_PTR g_gdiplusToken = 0;
HICON g_iconLarge = nullptr;
HICON g_iconSmall = nullptr;

std::vector<HWND> g_lobbyControls;
std::vector<HWND> g_roomControls;

SOCKET g_ipc = INVALID_SOCKET;
int g_guiPort = 0;
int g_dllPort = 0;
std::atomic<bool> g_ipcRunning{false};
HANDLE g_ipcThread = nullptr;
HANDLE g_relayReadyEvent = nullptr;

std::atomic<bool> g_hookReady{false};
std::atomic<bool> g_relayConnected{false};
std::atomic<uint32_t> g_selfVirt{0};
std::atomic<bool> g_busy{false};
std::wstring g_loginDisplayName;
std::wstring g_serverAddress;
int g_serverPort = 0;
bool g_verboseEnabled = false;

// GUI lobby connection.  This is independent of the hook IPC socket so the
// host/guest can join a room before the game is launched.
SOCKET g_lobbySocket = INVALID_SOCKET;
HANDLE g_lobbyThread = nullptr;
std::atomic<bool> g_lobbyRunning{false};
sockaddr_in g_lobbyServer{};
// Every lobby datagram is encrypted with this channel (X25519 + XSalsa20-Poly1305).
ngs::Channel g_lobbyChannel;
std::mutex g_lobbySecureMutex;
// Set by LobbyHandshake so StartLobbyNet can show a precise reason.
std::wstring g_lobbySecureError;
bool g_inLobby = false;
bool g_lobbyIsHost = false;
bool g_lobbyStarted = false;
bool g_lobbyWaiting = false;
int g_lobbyRetry = 0;
uint8_t g_pendingLobbyType = 0;
int g_roomMode = 1;
int g_lobbyMode = 1;
std::wstring g_lobbyRoom;
bool g_lobbyHasPassword = false;
std::vector<LobbyMemberInfo> g_lobbyMembers;
bool g_startingGame = false;

// Public room browser (home page) state.
std::vector<LobbyRoomInfo> g_roomList;
int g_roomListScroll = 0;
int g_hoverRoomRow = -1;
bool g_hoverJoinButton = false;
std::wstring g_pendingRoom;
std::wstring g_pendingPassword;

// Separate room window.  It owns the member list, kick buttons and start
// button; the main window stays on the room browser behind it.
int g_roomHoverKick = -1;
bool g_roomHoverStart = false;
bool g_roomHoverLeave = false;
HWND g_roomChatLog = nullptr;
HWND g_roomChatInput = nullptr;
HWND g_roomChatSend = nullptr;
std::vector<std::wstring> g_roomChatLines;
WNDPROC g_roomChatEditOldProc = nullptr;

// Modal room dialog state (create room / password prompt).
struct RoomDialogState {
    int kind = 0;                  // 0 = create, 1 = password
    bool done = false;
    bool ok = false;
    std::wstring room;
    std::wstring password;
    int mode = 1;
    HWND editRoom = nullptr;
    HWND editPassword = nullptr;
    HWND radios[5] = {};
};

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

std::wstring GetText(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring text((size_t)length + 1, L'\0');
    if (length > 0) GetWindowTextW(control, text.data(), length + 1);
    text.resize((size_t)length);
    return text;
}

void SetText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

std::wstring IpText(uint32_t ip) {
    return Utf8ToWide(fr::IpToString(ip));
}

std::wstring Trim(std::wstring text) {
    size_t first = text.find_first_not_of(L" \t\r\n");
    size_t last = text.find_last_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return text.substr(first, last - first + 1);
}

std::wstring DefaultNickname() {
    if (!g_loginDisplayName.empty()) return g_loginDisplayName;
    return L"Player";
}

std::wstring DefaultRoomName() {
    return L"Net11_" + DefaultNickname();
}

void SetStatus(const std::wstring& text);
void AppendLog(const std::wstring& message);
std::wstring ExecutableDirectory();
bool FileExists(const std::wstring& path);
bool StartLobbyNet(const std::wstring& server, int port);
void StopLobbyNet();
bool SendLobbyRequest(uint8_t type);
void SendLobbyPing();
void UpdateLobbyUi();
void LeaveLobby(bool notify);
void HandleLobbyEvent(const LobbyEvent& event);
void LaunchGameFlow();
void StartGameClicked();
void CloseRoomWindow();
void RefreshRoomWindow();
void UpdateRoomListUi();
void SendLobbyBrowse();
void JoinRoomByCode(const std::wstring& room, const std::wstring& password);
void JoinRowClicked(int index);
void KickMemberClicked(int index);
void OpenRoomWindow();
void AppendRoomChatLine(const std::wstring& line);
void ResetRoomChat();
void SendRoomChat();
void InvalidateRoomList();
bool ShowCreateRoomDialog(std::wstring& room, std::wstring& password, int& mode);
bool ShowPasswordDialog(const std::wstring& room, std::wstring& password);

std::wstring ClientConfigPath() {
    return ExecutableDirectory() + L"\\client_config.ini";
}

void LoadClientConfig() {
    std::wstring path = ClientConfigPath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 1024 * 1024) {
        CloseHandle(file);
        return;
    }
    std::string bytes(size, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, bytes.data(), size, &read, nullptr);
    CloseHandle(file);
    if (!ok) return;
    bytes.resize(read);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }
    std::wstring text = Utf8ToWide(bytes);
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring key = Trim(line.substr(0, eq));
            std::wstring value = Trim(line.substr(eq + 1));
            if (key == L"room" && !value.empty()) SetText(g_room, value);
            else if (key == L"password") SetText(g_password, value);
            else if (key == L"name" && !value.empty()) SetText(g_name, value);
            else if (key == L"game" && !value.empty()) SetText(g_game, value);
            else if (key == L"mode") {
                int mode = _wtoi(value.c_str());
                if (mode >= 1 && mode <= fr::kMaxLobbyMode) { g_roomMode = mode; g_lobbyMode = mode; }
            }
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    if (g_verbose) {
        InvalidateRect(g_verbose, nullptr, TRUE);
    }
}

std::string SanitizeConfigValue(const std::wstring& text) {
    std::string value = WideToUtf8(text);
    for (char& c : value) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return value;
}

void SaveClientConfig() {
    std::string bytes = "\xEF\xBB\xBF";
    bytes += "# Net Goal 11 client configuration\r\n";
    bytes += "room=" + SanitizeConfigValue(Trim(GetText(g_room))) + "\r\n";
    bytes += "password=" + SanitizeConfigValue(GetText(g_password)) + "\r\n";
    bytes += "name=" + SanitizeConfigValue(Trim(GetText(g_name))) + "\r\n";
    bytes += "game=" + SanitizeConfigValue(Trim(GetText(g_game))) + "\r\n";
    bytes += "mode=" + std::to_string(g_roomMode) + "\r\n";

    std::wstring path = ClientConfigPath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetStatus(L"Failed to save the settings");
        AppendLog(L"Failed to save the client config; check that the program folder is writable.");
        return;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(file);
    if (ok && written == bytes.size()) {
        SetStatus(L"Room, password, nickname and game path saved");
        AppendLog(L"Client config saved.");
    } else {
        SetStatus(L"Failed to save the settings");
        AppendLog(L"Failed to save the client config.");
    }
}

std::string TruncateUtf8(std::string text, size_t limit) {
    if (text.size() <= limit) return text;
    size_t n = limit;
    while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) --n;
    text.resize(n);
    return text;
}

// ---------------------------------------------------------------------------
// Thin dark scrollbar for the run-log edit
// ---------------------------------------------------------------------------
struct LogScrollMetrics {
    int minPos = 0;
    int maxPos = 0;
    int page = 0;
    int pos = 0;
    bool scrollable = false;
};

bool GetLogScrollMetrics(LogScrollMetrics& metrics) {
    metrics = LogScrollMetrics{};
    if (!g_log) return false;

    int totalLines = (int)SendMessageW(g_log, EM_GETLINECOUNT, 0, 0);
    if (totalLines < 1) return false;

    RECT client{};
    if (!GetClientRect(g_log, &client)) return false;

    int lineHeight = 0;
    if (totalLines > 1) {
        int firstIndex = (int)SendMessageW(g_log, EM_LINEINDEX, 0, 0);
        int secondIndex = (int)SendMessageW(g_log, EM_LINEINDEX, 1, 0);
        LRESULT firstPos = SendMessageW(g_log, EM_POSFROMCHAR, (WPARAM)firstIndex, 0);
        LRESULT secondPos = SendMessageW(g_log, EM_POSFROMCHAR, (WPARAM)secondIndex, 0);
        if (firstPos != (LRESULT)-1 && secondPos != (LRESULT)-1)
            lineHeight = HIWORD(secondPos) - HIWORD(firstPos);
    }
    if (lineHeight <= 0) {
        HDC dc = GetDC(g_log);
        if (dc) {
            HFONT font = (HFONT)SendMessageW(g_log, WM_GETFONT, 0, 0);
            HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
            TEXTMETRICW textMetrics{};
            if (GetTextMetricsW(dc, &textMetrics)) lineHeight = textMetrics.tmHeight;
            if (oldFont) SelectObject(dc, oldFont);
            ReleaseDC(g_log, dc);
        }
    }
    if (lineHeight <= 0) lineHeight = 16;

    int page = (client.bottom - client.top - 4) / lineHeight;
    if (page < 1) page = 1;
    if (page > totalLines) page = totalLines;

    int firstVisible = (int)SendMessageW(g_log, EM_GETFIRSTVISIBLELINE, 0, 0);
    metrics.minPos = 0;
    metrics.page = page;
    metrics.maxPos = totalLines - page;
    if (metrics.maxPos < 0) metrics.maxPos = 0;
    metrics.pos = firstVisible;
    if (metrics.pos < 0) metrics.pos = 0;
    if (metrics.pos > metrics.maxPos) metrics.pos = metrics.maxPos;
    metrics.scrollable = totalLines > page;
    return true;
}

void UpdateLogScrollbar() {
    if (g_logScroll && IsWindow(g_logScroll))
        InvalidateRect(g_logScroll, nullptr, FALSE);
}

void ScrollLogTo(int position) {
    LogScrollMetrics metrics;
    if (!GetLogScrollMetrics(metrics)) return;
    if (position < metrics.minPos) position = metrics.minPos;
    if (position > metrics.maxPos) position = metrics.maxPos;

    int current = (int)SendMessageW(g_log, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (position == current) return;
    SendMessageW(g_log, EM_LINESCROLL, 0, (LPARAM)(position - current));
    UpdateLogScrollbar();
}

bool GetLogThumbRect(HWND scrollbar, RECT& thumb, LogScrollMetrics& metrics) {
    RECT client{};
    if (!scrollbar || !GetClientRect(scrollbar, &client)) return false;
    if (!GetLogScrollMetrics(metrics) || !metrics.scrollable) return false;

    const int pad = 2;
    const int available = (client.bottom - client.top) - pad * 2;
    if (available <= 8) return false;

    const int span = metrics.maxPos - metrics.minPos + 1;
    int thumbHeight = MulDiv(available, metrics.page, span > 0 ? span : 1);
    if (thumbHeight < 16) thumbHeight = 16;
    if (thumbHeight > available) thumbHeight = available;

    const int range = metrics.maxPos - metrics.minPos;
    const int offset = range > 0
        ? MulDiv(available - thumbHeight, metrics.pos - metrics.minPos, range)
        : 0;

    thumb.left = client.left + 1;
    thumb.top = client.top + pad + offset;
    thumb.right = client.right - 1;
    thumb.bottom = thumb.top + thumbHeight;
    return true;
}

void PaintLogScrollbar(HWND hwnd, HDC dc) {
    RECT client{};
    GetClientRect(hwnd, &client);

    HBRUSH track = CreateSolidBrush(RGB(7, 8, 8));
    FillRect(dc, &client, track);
    DeleteObject(track);

    RECT edge{client.left, client.top, client.left + 1, client.bottom};
    HBRUSH edgeBrush = CreateSolidBrush(RGB(22, 19, 14));
    FillRect(dc, &edge, edgeBrush);
    DeleteObject(edgeBrush);

    RECT thumb{};
    LogScrollMetrics metrics;
    if (!GetLogThumbRect(hwnd, thumb, metrics)) return;

    COLORREF fill = g_scrollDragging ? RGB(158, 122, 58)
                  : g_scrollHover ? RGB(139, 108, 52)
                                  : RGB(114, 89, 44);
    COLORREF border = g_scrollDragging ? RGB(205, 166, 85) : RGB(82, 64, 30);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, thumb.left, thumb.top, thumb.right, thumb.bottom, 6, 6);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);

    HPEN highlight = CreatePen(PS_SOLID, 1, RGB(146, 115, 56));
    HGDIOBJ oldHighlight = SelectObject(dc, highlight);
    MoveToEx(dc, thumb.left + 2, thumb.top + 3, nullptr);
    LineTo(dc, thumb.left + 2, thumb.bottom - 3);
    SelectObject(dc, oldHighlight);
    DeleteObject(highlight);
}

void ApplyLogTextInset() {
    if (!g_log) return;
    RECT client{};
    if (!GetClientRect(g_log, &client)) return;

    RECT formatting = client;
    formatting.left += 2;
    formatting.top += 2;
    formatting.right -= kLogScrollWidth + kLogScrollInset + 3;
    formatting.bottom -= 2;
    if (formatting.right <= formatting.left || formatting.bottom <= formatting.top) return;
    SendMessageW(g_log, EM_SETRECT, 0, (LPARAM)&formatting);
}

void ApplyLogWindowRegion() {
    if (!g_log) return;
    RECT window{};
    if (!GetWindowRect(g_log, &window)) return;
    int width = window.right - window.left;
    int height = window.bottom - window.top;
    int clipWidth = width - (kLogScrollWidth + kLogScrollInset);
    if (clipWidth < 24 || height < 16) return;

    HRGN region = CreateRectRgn(0, 0, clipWidth, height);
    if (region) SetWindowRgn(g_log, region, TRUE);
}

void PositionLogScrollbar(HWND hwnd) {
    if (!g_log || !IsWindow(g_log)) return;

    bool show = IsWindowVisible(g_log) != FALSE && IsWindowVisible(g_hwnd) != FALSE;
    if (!show) {
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
        return;
    }

    RECT logRect{};
    if (!GetWindowRect(g_log, &logRect)) return;

    HWND parent = GetParent(hwnd);
    POINT points[2] = {{logRect.left, logRect.top}, {logRect.right, logRect.bottom}};
    MapWindowPoints(HWND_DESKTOP, parent, points, 2);

    const int x = points[1].x - kLogScrollInset - kLogScrollWidth;
    const int y = points[0].y + kLogScrollInset;
    const int w = kLogScrollWidth;
    const int h = (points[1].y - points[0].y) - kLogScrollInset * 2;
    if (w <= 0 || h <= 8) return;

    RECT current{};
    GetWindowRect(hwnd, &current);
    POINT currentPoints[2] = {{current.left, current.top}, {current.right, current.bottom}};
    MapWindowPoints(HWND_DESKTOP, parent, currentPoints, 2);
    bool same = currentPoints[0].x == x && currentPoints[0].y == y &&
                currentPoints[1].x - currentPoints[0].x == w &&
                currentPoints[1].y - currentPoints[0].y == h;

    if (!same)
        SetWindowPos(hwnd, HWND_TOP, x, y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else if (IsWindowVisible(hwnd))
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    else
        SetWindowPos(hwnd, HWND_TOP, x, y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void SyncLogScrollbar(HWND hwnd) {
    if (!g_log || !IsWindow(g_log)) return;
    PositionLogScrollbar(hwnd);

    RECT client{};
    if (GetClientRect(g_log, &client)) {
        static int lastWidth = 0;
        static int lastHeight = 0;
        if (client.right != lastWidth || client.bottom != lastHeight) {
            lastWidth = client.right;
            lastHeight = client.bottom;
            ApplyLogTextInset();
            ApplyLogWindowRegion();
        }
    }
}

LRESULT CALLBACK LogScrollProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 120, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == 1) {
            SyncLogScrollbar(hwnd);
            UpdateLogScrollbar();
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintLogScrollbar(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        LogScrollMetrics metrics;
        RECT thumb{};
        if (!GetLogThumbRect(hwnd, thumb, metrics)) return 0;

        int y = GET_Y_LPARAM(lParam);
        if (y >= thumb.top && y < thumb.bottom) {
            g_scrollDragging = true;
            g_scrollHover = true;
            g_scrollDragStartY = y;
            g_scrollDragStartPos = metrics.pos;

            RECT client{};
            GetClientRect(hwnd, &client);
            int thumbHeight = thumb.bottom - thumb.top;
            g_scrollDragTrackPixels = (client.bottom - client.top) - 4 - thumbHeight;
            if (g_scrollDragTrackPixels < 1) g_scrollDragTrackPixels = 1;
            g_scrollDragLineRange = metrics.maxPos - metrics.minPos;
            if (g_scrollDragLineRange < 1) g_scrollDragLineRange = 1;

            SetCapture(hwnd);
            SetFocus(g_log);
            UpdateLogScrollbar();
        } else if (y < thumb.top) {
            ScrollLogTo(metrics.pos - metrics.page);
        } else {
            ScrollLogTo(metrics.pos + metrics.page);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_scrollDragging) {
            int y = GET_Y_LPARAM(lParam);
            int delta = MulDiv(y - g_scrollDragStartY,
                               g_scrollDragLineRange, g_scrollDragTrackPixels);
            ScrollLogTo(g_scrollDragStartPos + delta);
        } else {
            LogScrollMetrics metrics;
            RECT thumb{};
            bool hover = GetLogThumbRect(hwnd, thumb, metrics) &&
                         GET_Y_LPARAM(lParam) >= thumb.top &&
                         GET_Y_LPARAM(lParam) < thumb.bottom;
            if (hover != g_scrollHover) {
                g_scrollHover = hover;
                UpdateLogScrollbar();
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_scrollDragging) {
            g_scrollDragging = false;
            ReleaseCapture();
            UpdateLogScrollbar();
        }
        return 0;

    case WM_MOUSELEAVE:
        if (!g_scrollDragging) {
            g_scrollHover = false;
            UpdateLogScrollbar();
        }
        return 0;

    case WM_MOUSEWHEEL: {
        LogScrollMetrics metrics;
        if (GetLogScrollMetrics(metrics)) {
            int lines = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 3;
            ScrollLogTo(metrics.pos - lines);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureLogScrollClass() {
    WNDCLASSEXW existing{sizeof(existing)};
    if (GetClassInfoExW(GetModuleHandleW(nullptr), kLogScrollClass, &existing)) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LogScrollProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kLogScrollClass;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    return RegisterClassExW(&wc) != 0;
}

void SetupLogScrollbar() {
    if (!g_log || !g_hwnd) return;
    EnsureLogScrollClass();

    LONG_PTR logStyle = GetWindowLongPtrW(g_log, GWL_STYLE);
    if (!(logStyle & WS_CLIPSIBLINGS))
        SetWindowLongPtrW(g_log, GWL_STYLE, logStyle | WS_CLIPSIBLINGS);

    ShowScrollBar(g_log, SB_VERT, FALSE);
    ApplyLogTextInset();
    ApplyLogWindowRegion();

    if (!g_logScroll || !IsWindow(g_logScroll)) {
        RECT logRect{};
        GetWindowRect(g_log, &logRect);
        POINT pt{logRect.left, logRect.top};
        MapWindowPoints(HWND_DESKTOP, g_hwnd, &pt, 1);
        int width = logRect.right - logRect.left;
        int height = logRect.bottom - logRect.top;
        g_logScroll = CreateWindowExW(
            0, kLogScrollClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            pt.x + width - kLogScrollInset - kLogScrollWidth,
            pt.y + kLogScrollInset,
            kLogScrollWidth, height - kLogScrollInset * 2,
            g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    if (g_logScroll) {
        PositionLogScrollbar(g_logScroll);
        UpdateLogScrollbar();
    }
}

void AppendLog(const std::wstring& message) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t prefix[64];
    swprintf(prefix, 64, L"[%02d-%02d-%02d %02d:%02d:%02d] ",
             st.wYear % 100, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = prefix + message + L"\r\n";
    int length = GetWindowTextLengthW(g_log);
    if (length > 200000) {
        SetWindowTextW(g_log, L"... log cleared (too long) ...\r\n");
        length = GetWindowTextLengthW(g_log);
    }
    SendMessageW(g_log, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
    UpdateLogScrollbar();
}

bool SendIpc(uint8_t type, uint8_t kind, uint32_t dstVirt, uint16_t srcPort,
             uint16_t dstPort, const void* payload, uint16_t length);
void SetStatus(const std::wstring& text);

void AppendChatLine(const std::wstring& name, const std::wstring& text) {
    if (!g_chatLog || text.empty()) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t prefix[64];
    swprintf(prefix, 64, L"[%02d-%02d-%02d %02d:%02d:%02d] ",
             st.wYear % 100, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = prefix + name + L": " + text + L"\r\n";
    int length = GetWindowTextLengthW(g_chatLog);
    if (length > 100000) {
        SetWindowTextW(g_chatLog, L"... chat log cleared (too long) ...\r\n");
        length = GetWindowTextLengthW(g_chatLog);
    }
    SendMessageW(g_chatLog, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageW(g_chatLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g_chatLog, EM_SCROLLCARET, 0, 0);
}

void SendChat() {
    if (!g_chatInput) return;
    std::wstring text = Trim(GetText(g_chatInput));
    if (text.empty()) return;
    if (!g_hookReady.load() || !g_relayConnected.load()) {
        AppendChatLine(L"System", L"Not connected to the relay; chat not sent");
        SetStatus(L"Not connected to the relay; cannot send chat");
        return;
    }
    std::string utf8 = WideToUtf8(text);
    if (utf8.empty()) return;
    if (utf8.size() > fr::kMaxChatText) {
        utf8 = TruncateUtf8(utf8, fr::kMaxChatText);
    }
    if (!SendIpc(fr::IpcChat, 0, 0, 0, 0, utf8.data(), (uint16_t)utf8.size())) {
        AppendChatLine(L"System", L"Chat send failed; the Hook is not connected");
        SetStatus(L"Chat send failed");
        return;
    }
    SetText(g_chatInput, L"");
    SetFocus(g_chatInput);
}

void SetStatus(const std::wstring& text) {
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

void ShowTab(int index) {
    g_activeTab = index == 1 ? 1 : 0;
    for (HWND control : g_lobbyControls) {
        ShowWindow(control, g_activeTab == 0 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_roomControls) {
        ShowWindow(control, g_activeTab == 1 ? SW_SHOW : SW_HIDE);
    }
    if (g_tabLobby) InvalidateRect(g_tabLobby, nullptr, TRUE);
    if (g_tabRoom) InvalidateRect(g_tabRoom, nullptr, TRUE);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE);
}

std::wstring ModeText(int mode) {
    switch (mode) {
        case 2: return L"2v2";
        case 3: return L"3v3";
        case 4: return L"4v4";
        case 5: return L"5v5";
        default: return L"1v1";
    }
}

void UpdateRoomTree() {
    if (!g_tree || !g_treeRoomItem) return;
    std::wstring gameText = L"Net Goal 11";
    TVITEMW item{};
    item.mask = TVIF_TEXT;
    item.hItem = g_treeRoomItem;
    item.pszText = gameText.data();
    item.cchTextMax = (int)gameText.size();
    TreeView_SetItem(g_tree, &item);

    if (!g_inLobby) {
        if (g_treeLobbyItem) {
            TreeView_DeleteItem(g_tree, g_treeLobbyItem);
            g_treeLobbyItem = nullptr;
        }
        return;
    }

    std::wstring roomText = g_lobbyRoom + L"  [" + ModeText(g_lobbyMode) + L"]";
    if (!g_treeLobbyItem) {
        TVINSERTSTRUCTW insert{};
        insert.hParent = g_treeRoomItem;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT;
        insert.item.pszText = roomText.data();
        insert.item.cchTextMax = (int)roomText.size();
        g_treeLobbyItem = TreeView_InsertItem(g_tree, &insert);
        TreeView_Expand(g_tree, g_treeRoomItem, TVE_EXPAND);
    } else {
        TVITEMW roomItem{};
        roomItem.mask = TVIF_TEXT;
        roomItem.hItem = g_treeLobbyItem;
        roomItem.pszText = roomText.data();
        roomItem.cchTextMax = (int)roomText.size();
        TreeView_SetItem(g_tree, &roomItem);
    }
}

void UpdateLobbyUi() {
    UpdateRoomTree();
    bool idle = !g_inLobby && !g_lobbyWaiting;
    if (g_newRoom) EnableWindow(g_newRoom, idle);
    if (g_refreshRooms) EnableWindow(g_refreshRooms, idle);
    if (g_name) EnableWindow(g_name, true);
    if (g_game) EnableWindow(g_game, true);
    if (g_browse) EnableWindow(g_browse, true);
    if (g_saveConfig) EnableWindow(g_saveConfig, true);
    if (g_roomWindow) RefreshRoomWindow();
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE);
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void BrowseGame() {
    wchar_t file[MAX_PATH] = {};
    std::wstring current = Trim(GetText(g_game));
    if (!current.empty()) wcscpy_s(file, current.c_str());

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hwnd;
    dialog.lpstrFilter = L"Game executable (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Select the game executable";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        SetText(g_game, file);
        AppendLog(L"Selected game: " + std::wstring(file));
    }
}

// ---------------------------------------------------------------------------
// Local IPC (control/status only)
// ---------------------------------------------------------------------------
bool SendIpc(uint8_t type, uint8_t kind, uint32_t dstVirt, uint16_t srcPort,
             uint16_t dstPort, const void* payload, uint16_t length) {
    if (g_ipc == INVALID_SOCKET || g_dllPort == 0 || length > fr::kMaxPayload) return false;
    uint8_t buffer[sizeof(fr::IpcHeader) + fr::kMaxPayload];
    fr::IpcHeader header{};
    header.magic = fr::kIpcMagic;
    header.type = type;
    header.kind = kind;
    header.dstVirt = dstVirt;
    header.srcPort = srcPort;
    header.dstPort = dstPort;
    header.length = length;
    memcpy(buffer, &header, sizeof(header));
    if (length && payload) memcpy(buffer + sizeof(header), payload, length);

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = inet_addr("127.0.0.1");
    target.sin_port = htons((u_short)g_dllPort);
    int sent = sendto(g_ipc, (const char*)buffer, (int)(sizeof(header) + length), 0,
                      (const sockaddr*)&target, sizeof(target));
    return sent == (int)(sizeof(header) + length);
}

DWORD WINAPI IpcThread(LPVOID) {
    uint8_t buffer[2048];
    while (g_ipcRunning.load(std::memory_order_relaxed)) {
        sockaddr_in from{};
        int fromLength = sizeof(from);
        int received = recvfrom(g_ipc, (char*)buffer, sizeof(buffer), 0,
                                (sockaddr*)&from, &fromLength);
        if (received == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) continue;
            if (error == WSAENOTSOCK) break;
            if (error == WSAECONNRESET) continue;
            continue;
        }
        if (received < (int)sizeof(fr::IpcHeader)) continue;
        fr::IpcHeader header{};
        memcpy(&header, buffer, sizeof(header));
        if (header.magic != fr::kIpcMagic) continue;
        if (header.length > received - (int)sizeof(header)) continue;
        // LaunchAndInject blocks the UI thread waiting for this event, so it
        // must be signaled here in the IPC thread, not only from WM_APP.
        if (header.type == fr::IpcRelayReady && g_relayReadyEvent)
            SetEvent(g_relayReadyEvent);
        auto* event = new HookEvent();
        event->type = header.type;
        event->payload.assign((const char*)buffer + sizeof(header), header.length);
        PostMessageW(g_hwnd, WM_APP_HOOK_EVENT, 0, (LPARAM)event);
    }
    return 0;
}

bool StartIpc() {
    if (g_ipc != INVALID_SOCKET) return true;
    for (int offset = 0; offset < 80; offset += 2) {
        int guiPort = fr::kDefaultGuiPort + offset;
        int dllPort = guiPort + 1;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return false;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = inet_addr("127.0.0.1");
        local.sin_port = htons((u_short)guiPort);
        if (bind(s, (const sockaddr*)&local, sizeof(local)) != 0) {
            closesocket(s);
            continue;
        }
        DWORD timeout = 500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        BOOL noReset = FALSE;
        DWORD ignored = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), nullptr, 0, &ignored, nullptr, nullptr);
        g_ipc = s;
        g_guiPort = guiPort;
        g_dllPort = dllPort;
        g_ipcRunning.store(true, std::memory_order_relaxed);
        g_ipcThread = CreateThread(nullptr, 0, IpcThread, nullptr, 0, nullptr);
        if (!g_ipcThread) {
            g_ipcRunning.store(false);
            closesocket(g_ipc);
            g_ipc = INVALID_SOCKET;
            return false;
        }
        AppendLog(L"Local IPC listening on 127.0.0.1:" + std::to_wstring(guiPort) +
                  L", Hook port " + std::to_wstring(dllPort));
        return true;
    }
    AppendLog(L"Cannot bind a local IPC port in 45889-45967");
    return false;
}

void StopIpc() {
    g_ipcRunning.store(false, std::memory_order_relaxed);
    if (g_ipc != INVALID_SOCKET) {
        closesocket(g_ipc);
        g_ipc = INVALID_SOCKET;
    }
    if (g_ipcThread) {
        WaitForSingleObject(g_ipcThread, 1000);
        CloseHandle(g_ipcThread);
        g_ipcThread = nullptr;
    }
}

// ---------------------------------------------------------------------------
// GUI lobby (create/join/start, no game traffic)
// ---------------------------------------------------------------------------
DWORD WINAPI LobbyThread(LPVOID) {
    uint8_t buffer[2048];
    while (g_lobbyRunning.load(std::memory_order_relaxed)) {
        sockaddr_in from{};
        int fromLength = sizeof(from);
        int received = recvfrom(g_lobbySocket, (char*)buffer, sizeof(buffer), 0,
                                (sockaddr*)&from, &fromLength);
        if (received == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) continue;
            if (error == WSAENOTSOCK) break;
            if (error == WSAECONNRESET) continue;
            continue;
        }
        if (!ngs::IsSecureFrame(buffer, (size_t)received)) continue;
        if (buffer[5] != ngs::FrameData) continue;
        uint8_t plain[ngs::kMaxPlain];
        size_t plainLength = 0;
        {
            std::lock_guard<std::mutex> lock(g_lobbySecureMutex);
            if (!g_lobbyChannel.Open(buffer, (size_t)received, plain, plainLength)) continue;
        }
        if (plainLength < fr::kHeaderSize) continue;
        fr::RelayHeader header{};
        if (!fr::DecodeHeader(plain, plainLength, header)) continue;
        if (header.type != fr::LobbyState && header.type != fr::LobbyStart &&
            header.type != fr::LobbyList && header.type != fr::LobbyChatMessage &&
            header.type != fr::Error) {
            continue;
        }
        auto* event = new LobbyEvent();
        event->type = header.type;
        event->payload.assign((const char*)plain + fr::kHeaderSize, header.length);
        if (g_hwnd && IsWindow(g_hwnd)) {
            PostMessageW(g_hwnd, WM_APP_LOBBY_EVENT, 0, (LPARAM)event);
        } else {
            delete event;
        }
    }
    return 0;
}

// Blocking handshake performed right before the lobby thread starts.  The
// socket already has a 500 ms receive timeout, so this never stalls for long.
// The server authenticates itself with its pinned long-term key (TOFU).
bool LobbyHandshake(SOCKET s) {
    std::lock_guard<std::mutex> lock(g_lobbySecureMutex);
    g_lobbySecureError.clear();
    g_lobbyChannel.BeginClientHandshake(GetTickCount64());
    uint8_t init[ngs::kMaxFrame];
    size_t initLength = 0;
    g_lobbyChannel.BuildInit(init, initLength);

    char endpointText[64];
    snprintf(endpointText, sizeof(endpointText), "%s:%u",
             fr::IpToString(g_lobbyServer.sin_addr.s_addr).c_str(),
             (unsigned)ntohs(g_lobbyServer.sin_port));
    const std::string endpoint = endpointText;

    ULONGLONG deadline = GetTickCount64() + 3000;
    ULONGLONG nextSend = 0;
    while (GetTickCount64() < deadline) {
        ULONGLONG now = GetTickCount64();
        if (now >= nextSend) {
            sendto(s, (const char*)init, (int)initLength, 0,
                   (const sockaddr*)&g_lobbyServer, sizeof(g_lobbyServer));
            nextSend = now + 500;
        }
        uint8_t buffer[ngs::kMaxFrame];
        int received = recvfrom(s, (char*)buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (received < (int)ngs::kHeaderSize) continue;
        if (!ngs::IsSecureFrame(buffer, (size_t)received)) continue;
        if (buffer[5] != ngs::FrameReply) continue;

        uint8_t serverKey[32] = {};
        if (!ngs::Channel::PeekReplyStatic(buffer, (size_t)received, serverKey)) continue;

        uint8_t pinned[32] = {};
        bool known = ngp::LoadPin(endpoint, pinned);
        if (known && !ngc::ConstantTimeEqual(pinned, serverKey, 32)) {
            g_lobbySecureError =
                L"The relay server encryption key changed. Pinned " +
                Utf8ToWide(ngp::Fingerprint(pinned)) + L", received " +
                Utf8ToWide(ngp::Fingerprint(serverKey)) + L". Delete " +
                Utf8ToWide(ngp::StorePath()) + L" if the server was reinstalled.";
            return false;
        }

        if (!g_lobbyChannel.HandleReply(buffer, (size_t)received, serverKey, GetTickCount64()))
            continue;

        if (!known) {
            if (ngp::SavePin(endpoint, serverKey))
                AppendLog(L"Pinned the relay server key " +
                          Utf8ToWide(ngp::Fingerprint(serverKey)) + L" for " +
                          Utf8ToWide(endpoint) + L".");
            else
                AppendLog(L"Warning: the pinned server key could not be saved.");
        }
        return true;
    }
    return false;
}

bool StartLobbyNet(const std::wstring& server, int port) {
    if (g_lobbySocket != INVALID_SOCKET) return true;

    std::string host = WideToUtf8(server);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    char portText[16];
    snprintf(portText, sizeof(portText), "%d", port);
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), portText, &hints, &result) != 0 || !result) {
        AppendLog(L"Cannot resolve the online server address.");
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        AppendLog(L"Failed to create the room socket.");
        return false;
    }
    DWORD timeout = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    BOOL noReset = FALSE;
    DWORD ignored = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), nullptr, 0, &ignored, nullptr, nullptr);

    g_lobbySocket = s;
    memset(&g_lobbyServer, 0, sizeof(g_lobbyServer));
    memcpy(&g_lobbyServer, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (!LobbyHandshake(s)) {
        closesocket(s);
        g_lobbySocket = INVALID_SOCKET;
        memset(&g_lobbyServer, 0, sizeof(g_lobbyServer));
        AppendLog(g_lobbySecureError.empty()
                      ? std::wstring(L"The encrypted handshake with the server failed.")
                      : g_lobbySecureError);
        return false;
    }

    g_lobbyRunning.store(true, std::memory_order_relaxed);
    g_lobbyThread = CreateThread(nullptr, 0, LobbyThread, nullptr, 0, nullptr);
    if (!g_lobbyThread) {
        g_lobbyRunning.store(false);
        closesocket(g_lobbySocket);
        g_lobbySocket = INVALID_SOCKET;
        AppendLog(L"Failed to create the room thread.");
        return false;
    }
    return true;
}

void StopLobbyNet() {
    g_lobbyRunning.store(false, std::memory_order_relaxed);
    if (g_lobbySocket != INVALID_SOCKET) {
        closesocket(g_lobbySocket);
        g_lobbySocket = INVALID_SOCKET;
    }
    if (g_lobbyThread) {
        WaitForSingleObject(g_lobbyThread, 1000);
        CloseHandle(g_lobbyThread);
        g_lobbyThread = nullptr;
    }
    memset(&g_lobbyServer, 0, sizeof(g_lobbyServer));
}

bool SendLobbyPacket(uint8_t type, const void* payload, uint16_t length) {
    if (g_lobbySocket == INVALID_SOCKET || g_lobbyServer.sin_family != AF_INET) return false;
    uint8_t buffer[sizeof(fr::RelayHeader) + fr::kMaxPayload];
    if (!fr::EncodeHeader(buffer, (fr::MessageType)type, 0, 0, 0, 0, 0, length)) return false;
    if (length && payload) memcpy(buffer + fr::kHeaderSize, payload, length);
    uint8_t frame[ngs::kMaxFrame];
    size_t frameLength = 0;
    {
        std::lock_guard<std::mutex> lock(g_lobbySecureMutex);
        if (!g_lobbyChannel.ready) return false;
        if (!g_lobbyChannel.Seal(buffer, fr::kHeaderSize + length, frame, frameLength)) return false;
    }
    int sent = sendto(g_lobbySocket, (const char*)frame, (int)frameLength, 0,
                      (const sockaddr*)&g_lobbyServer, sizeof(g_lobbyServer));
    return sent == (int)frameLength;
}

bool BuildLobbyRequest(fr::LobbyRequestPayload& request, uint8_t mode) {
    std::wstring room = g_pendingRoom;
    if (room.empty()) room = DefaultRoomName();
    std::wstring name = Trim(GetText(g_name));
    if (name.empty()) name = DefaultNickname();
    std::wstring game = Trim(GetText(g_game));
    std::string roomUtf8 = TruncateUtf8(WideToUtf8(room), fr::kRoomLength);
    std::string nameUtf8 = TruncateUtf8(WideToUtf8(name), fr::kNameLength);
    uint64_t passwordHash = fr::PasswordHash(WideToUtf8(g_pendingPassword));
    uint8_t raw[sizeof(fr::LobbyRequestPayload)];
    fr::EncodeLobbyRequest(raw, mode, FileExists(game), passwordHash,
                           roomUtf8.c_str(), nameUtf8);
    memcpy(&request, raw, sizeof(request));
    return true;
}

bool SendLobbyRequest(uint8_t type) {
    fr::LobbyRequestPayload request{};
    BuildLobbyRequest(request, (uint8_t)g_roomMode);
    return SendLobbyPacket(type, &request, sizeof(request));
}

void SendLobbyPing() {
    if (!g_inLobby) return;
    SendLobbyRequest(fr::LobbyPing);
    // Keep the public room list fresh even while we are already in a room, so
    // the newly created room (and member counts) show up on the home page.
    SendLobbyBrowse();
}

void SendLobbyBrowse() {
    SendLobbyPacket(fr::LobbyBrowse, nullptr, 0);
}

void InvalidateRoomList() {
    if (!g_hwnd) return;
    RECT rc{kRoomListLeft - 2, kRoomListTop - 2, kRoomListRight + 2,
            kRoomListTop + kRoomRowHeight * kRoomListVisible + 28};
    InvalidateRect(g_hwnd, &rc, FALSE);
}

// ---------------------------------------------------------------------------
// Hook deployment
// ---------------------------------------------------------------------------
std::wstring PrepareHookFiles(bool local = false) {
    std::wstring exeDir = ExecutableDirectory();
    std::wstring source = exeDir + L"\\Net Goal 11.Hook.dll";
    if (!FileExists(source)) {
        source = exeDir + L"\\..\\dist\\x86\\Net Goal 11.Hook.dll";
    }
    if (!FileExists(source)) {
        throw std::runtime_error("The client folder is missing Net Goal 11.Hook.dll");
    }

    wchar_t tempRoot[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempRoot);
    std::wstring base = std::wstring(tempRoot) + L"NetGoal11Native";
    CreateDirectoryW(base.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t dirName[128];
    swprintf(dirName, 128, L"\\%04d%02d%02d_%02d%02d%02d_%lu",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             GetCurrentProcessId());
    std::wstring directory = base + dirName;
    CreateDirectoryW(directory.c_str(), nullptr);

    std::wstring dllPath = directory + L"\\Net Goal 11.Hook.dll";
    if (!CopyFileW(source.c_str(), dllPath.c_str(), FALSE)) {
        throw std::runtime_error("Failed to copy the Hook DLL");
    }

    std::wstring serverText = g_serverAddress;
    if (serverText.empty()) serverText = L"127.0.0.1";
    std::wstring roomText = g_lobbyRoom;
    if (roomText.empty()) roomText = Trim(GetText(g_room));
    if (roomText.empty()) roomText = DefaultRoomName();
    std::wstring nameText = Trim(GetText(g_name));
    if (nameText.empty()) nameText = DefaultNickname();
    std::wstring passwordText = g_pendingPassword;
    if (passwordText.empty()) passwordText = GetText(g_password);

    int port = g_serverPort;
    if (port < 1 || port > 65535) port = fr::kDefaultPort;

    std::string roomUtf8 = TruncateUtf8(WideToUtf8(roomText), fr::kRoomLength);
    std::string nameUtf8 = TruncateUtf8(WideToUtf8(nameText), fr::kNameLength);
    std::string passwordUtf8 = WideToUtf8(passwordText);
    uint64_t passwordHash = fr::PasswordHash(passwordUtf8);

    wchar_t hashText[64];
    swprintf(hashText, 64, L"0x%016llx", (unsigned long long)passwordHash);

    std::wstring ini;
    ini += L"[hook]\r\n";
    ini += L"gui_port=" + std::to_wstring(g_guiPort) + L"\r\n";
    ini += L"dll_port=" + std::to_wstring(g_dllPort) + L"\r\n";
    ini += local ? L"direct=0\r\n" : L"direct=1\r\n";
    ini += L"server=" + serverText + L"\r\n";
    ini += L"server_port=" + std::to_wstring(port) + L"\r\n";
    ini += L"room=" + Utf8ToWide(roomUtf8) + L"\r\n";
    ini += L"password_hash=" + std::wstring(hashText) + L"\r\n";
    ini += L"name=" + Utf8ToWide(nameUtf8) + L"\r\n";
    ini += L"virtual_subnet=10.213.77.0\r\n";
    ini += L"virtual_mask=255.255.255.0\r\n";
    ini += L"virtual_ip=0\r\n";
    ini += L"local_ip=0.0.0.0\r\n";
    int logLevel = g_verboseEnabled ? 2 : 1;
    ini += L"log_level=" + std::to_wstring(logLevel) + L"\r\n";
    ini += L"log_path=\r\n";

    std::wstring iniPath = directory + L"\\Net Goal 11.Hook.ini";
    HANDLE file = CreateFileW(iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Failed to write the Hook config");
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD written = 0;
    WriteFile(file, bom, 2, &written, nullptr);
    WriteFile(file, ini.data(), (DWORD)(ini.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);

    return dllPath;
}

// ---------------------------------------------------------------------------
// DLL injection
// ---------------------------------------------------------------------------
bool InjectDll(HANDLE process, const std::wstring& dllPath, std::wstring& error) {
    std::wstring full = dllPath;
    size_t bytes = (full.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { error = L"Failed to allocate remote memory"; return false; }
    bool ok = false;
    HANDLE thread = nullptr;
    do {
        if (!WriteProcessMemory(process, remote, full.c_str(), bytes, nullptr)) {
            error = L"Failed to write the DLL path";
            break;
        }
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW");
        if (!loadLibrary) { error = L"LoadLibraryW was not found"; break; }
        thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (!thread) { error = L"Failed to create the remote thread"; break; }
        DWORD wait = WaitForSingleObject(thread, 10000);
        if (wait != WAIT_OBJECT_0) { error = L"Timed out injecting the DLL"; break; }
        DWORD exitCode = 0;
        if (!GetExitCodeThread(thread, &exitCode) || exitCode == 0) {
            error = L"The game failed to load the DLL";
            break;
        }
        ok = true;
    } while (false);
    if (thread) CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

bool WaitForRelayReady(DWORD timeoutMs) {
    DWORD start = GetTickCount();
    while (true) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) return false;
        DWORD wait = MsgWaitForMultipleObjects(1, &g_relayReadyEvent, FALSE,
                                               timeoutMs - elapsed, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) return true;
        if (wait == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        return false;
    }
}

bool LaunchAndInject(const std::wstring& gamePath, const std::wstring& dllPath,
                     std::wstring& error, bool waitForRelay = true) {
    std::wstring workDir = gamePath;
    size_t slash = workDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir.resize(slash);
    std::wstring commandLine = L"\"" + gamePath + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(gamePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, workDir.c_str(), &startup, &process)) {
        error = L"Failed to start the game";
        return false;
    }

    bool ok = false;
    do {
        if (!InjectDll(process.hProcess, dllPath, error)) break;
        // Wait until the hook has joined the room.  The game main thread is
        // still suspended, so it cannot cache the physical adapter address.
        // Pump UI messages while waiting so the client does not look frozen.
        if (waitForRelay && !WaitForRelayReady(10000)) {
            error = L"The Hook did not join the room within 10 seconds. Single-player rooms are allowed too; make sure the server, client and Hook are all up to date.";
            break;
        }
        if (ResumeThread(process.hThread) == (DWORD)-1) {
            error = L"Failed to resume the game thread";
            break;
        }
        ok = true;
    } while (false);

    if (!ok) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ok;
}

bool AttachAndInject(const std::wstring& dllPath, std::wstring& error) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { error = L"Failed to enumerate processes"; return false; }
    DWORD pid = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, fr::GameExecutableName().c_str()) == 0) { pid = entry.th32ProcessID; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!pid) { error = L"The game process was not found; start the game first"; return false; }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) { error = L"Failed to open the game process (make sure both run as the same user)"; return false; }
    bool ok = InjectDll(process, dllPath, error);
    CloseHandle(process);
    return ok;
}

// ---------------------------------------------------------------------------
// Hook events
// ---------------------------------------------------------------------------
void HandleHookEvent(const HookEvent& event) {
    switch (event.type) {
        case fr::IpcReady:
            g_hookReady.store(true);
            AppendLog(L"Hook ready; Winsock interception installed.");
            break;
        case fr::IpcRelayReady: {
            g_hookReady.store(true);
            if (event.payload.size() >= 4) {
                uint32_t self = fr::Read32(event.payload.data());
                g_selfVirt.store(self);
            }
            SetEvent(g_relayReadyEvent);
            std::wstring text = L"Hook connected to the relay server; LAN mode is ready.";
            if (g_selfVirt.load()) text += L" local virtual IP " + IpText(g_selfVirt.load());
            AppendLog(text);
            SetStatus(text);
            break;
        }
        case fr::IpcStatus:
            if (event.payload.size() >= sizeof(fr::IpcStatusPayload)) {
                fr::IpcStatusPayload status{};
                memcpy(&status, event.payload.data(), sizeof(status));
                g_selfVirt.store(status.selfVirt);
                g_relayConnected.store(status.state == fr::RelayConnected);
                std::wstring text = Utf8ToWide(std::string(status.text, strnlen(status.text, sizeof(status.text))));
                if (status.selfVirt) text += L" (virtual IP " + IpText(status.selfVirt) + L")";
                SetStatus(text);
                AppendLog(L"Relay: " + text);
            }
            break;
        case fr::IpcPeers: {
            int count = 0;
            if (event.payload.size() >= 1) {
                int n = (uint8_t)event.payload[0];
                if (n >= 0 && n <= (int)fr::kMaxPeers) {
                    size_t off = 1;
                    std::vector<std::wstring> rows;
                    uint32_t self = g_selfVirt.load();
                    for (int i = 0; i < n; ++i) {
                        if (off + 5 > event.payload.size()) break;
                        uint32_t ip = fr::Read32(event.payload.data() + off);
                        off += 4;
                        uint8_t nameLength = (uint8_t)event.payload[off++];
                        if (off + nameLength > event.payload.size()) break;
                        std::string name = event.payload.substr(off, nameLength);
                        off += nameLength;
                        std::wstring row = Utf8ToWide(name);
                        row += L"    ";
                        row += IpText(ip);
                        if (ip == self) row += L"  (you)";
                        rows.push_back(row);
                        ++count;
                    }
                    if (g_peers) {
                        SendMessageW(g_peers, LB_RESETCONTENT, 0, 0);
                        for (const auto& row : rows)
                            SendMessageW(g_peers, LB_ADDSTRING, 0, (LPARAM)row.c_str());
                    }
                }
            }
            if (count == 0 && g_peers) SendMessageW(g_peers, LB_RESETCONTENT, 0, 0);
            break;
        }
        case fr::IpcChat: {
            std::string name;
            std::string text;
            if (fr::DecodeChat(reinterpret_cast<const uint8_t*>(event.payload.data()),
                               event.payload.size(), name, text)) {
                AppendChatLine(Utf8ToWide(name), Utf8ToWide(text));
            }
            break;
        }
        case fr::IpcStats:
            if (event.payload.size() >= sizeof(fr::IpcStatsPayload)) {
                fr::IpcStatsPayload stats{};
                memcpy(&stats, event.payload.data(), sizeof(stats));
                wchar_t text[256];
                swprintf(text, 256, L"Up %llu / %llu KB   Down %llu / %llu KB",
                         (unsigned long long)stats.upPackets,
                         (unsigned long long)(stats.upBytes / 1024),
                         (unsigned long long)stats.downPackets,
                         (unsigned long long)(stats.downBytes / 1024));
                SetWindowTextW(g_stats, text);
            }
            break;
        case fr::IpcLog:
            AppendLog(Utf8ToWide(event.payload));
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
void SetPeerHint(const wchar_t* text) {
    if (!g_peers) return;
    SendMessageW(g_peers, LB_RESETCONTENT, 0, 0);
    if (text && *text)
        SendMessageW(g_peers, LB_ADDSTRING, 0, (LPARAM)text);
}

bool ReadSettings(std::wstring& server, int& port, std::wstring& room, std::wstring& name,
                  std::wstring& game) {
    server = g_serverAddress;
    port = g_serverPort;
    if (server.empty() || port < 1 || port > 65535) {
        MessageBoxW(g_hwnd, L"The server address in the settings is invalid. Please sign in again.",
                    L"Settings error", MB_OK | MB_ICONERROR);
        return false;
    }
    room = Trim(GetText(g_room));
    if (room.empty()) room = DefaultRoomName();
    name = Trim(GetText(g_name));
    if (name.empty()) name = DefaultNickname();
    game = Trim(GetText(g_game));
    return true;
}

void LeaveLobby(bool notify) {
    if (notify && g_inLobby) {
        SendLobbyPacket(fr::LobbyLeave, nullptr, 0);
    }
    g_inLobby = false;
    g_lobbyIsHost = false;
    g_lobbyStarted = false;
    g_lobbyWaiting = false;
    g_pendingLobbyType = 0;
    g_lobbyMembers.clear();
    g_lobbyRoom.clear();
    g_lobbyHasPassword = false;
    g_pendingRoom.clear();
    g_pendingPassword.clear();
    g_lobbyMode = g_roomMode;
    ResetRoomChat();
    CloseRoomWindow();
    if (notify) SetStatus(L"Left the room");
    if (g_hwnd) SetTimer(g_hwnd, IDT_LOBBY_TIMER, 2000, nullptr);
    SendLobbyBrowse();
    UpdateLobbyUi();
}

void CreateRoomClicked() {
    if (g_busy.exchange(true)) return;
    if (g_inLobby || g_lobbyWaiting) { g_busy.store(false); return; }
    std::wstring room, password;
    int mode = g_roomMode;
    if (!ShowCreateRoomDialog(room, password, mode)) { g_busy.store(false); return; }
    room = Trim(room);
    if (room.empty()) {
        MessageBoxW(g_hwnd, L"Enter a room code.", L"New Room", MB_OK | MB_ICONWARNING);
        g_busy.store(false);
        return;
    }
    if (!StartLobbyNet(g_serverAddress, g_serverPort)) { g_busy.store(false); return; }

    ResetRoomChat();
    g_roomMode = mode;
    g_lobbyMode = mode;
    g_lobbyHasPassword = !password.empty();
    g_pendingRoom = room;
    g_pendingPassword = password;
    g_lobbyRoom = room;
    g_lobbyMembers.clear();
    g_lobbyWaiting = true;
    g_pendingLobbyType = fr::LobbyCreate;
    g_lobbyRetry = 0;
    if (!SendLobbyRequest(fr::LobbyCreate)) {
        SetStatus(L"Failed to create the room: room channel not ready");
        AppendLog(L"Failed to create the room; the room channel is not ready.");
        g_lobbyWaiting = false;
        g_pendingLobbyType = 0;
        g_busy.store(false);
        return;
    }
    SetStatus(L"Creating room " + room + L" (" + ModeText(mode) + L")...");
    AppendLog(L"Create room " + room + L", mode " + ModeText(mode));
    if (!FileExists(Trim(GetText(g_game)))) {
        AppendLog(L"Note: no local game path. Choose the game executable on the right of the home page first.");
    }
    SetTimer(g_hwnd, IDT_LOBBY_TIMER, 5000, nullptr);
    UpdateLobbyUi();
    g_busy.store(false);
}

void JoinRoomByCode(const std::wstring& room, const std::wstring& password) {
    if (g_busy.exchange(true)) return;
    if (g_inLobby || g_lobbyWaiting) { g_busy.store(false); return; }
    if (!StartLobbyNet(g_serverAddress, g_serverPort)) { g_busy.store(false); return; }

    ResetRoomChat();
    g_pendingRoom = room;
    g_pendingPassword = password;
    g_lobbyHasPassword = !password.empty();
    g_lobbyRoom = room;
    g_lobbyMembers.clear();
    g_lobbyWaiting = true;
    g_pendingLobbyType = fr::LobbyJoin;
    g_lobbyRetry = 0;
    if (!SendLobbyRequest(fr::LobbyJoin)) {
        SetStatus(L"Failed to join: room channel not ready");
        AppendLog(L"Failed to join; the room channel is not ready.");
        g_lobbyWaiting = false;
        g_pendingLobbyType = 0;
        g_busy.store(false);
        return;
    }
    SetStatus(L"Joining room " + room + L"...");
    AppendLog(L"Join room " + room);
    if (!FileExists(Trim(GetText(g_game)))) {
        AppendLog(L"Note: no local game path; the host cannot start the game. Choose the game executable first.");
    }
    SetTimer(g_hwnd, IDT_LOBBY_TIMER, 5000, nullptr);
    UpdateLobbyUi();
    g_busy.store(false);
}

void JoinRowClicked(int index) {
    if (index < 0 || index >= static_cast<int>(g_roomList.size())) return;
    if (g_lobbyWaiting) return;
    const LobbyRoomInfo info = g_roomList[index];
    if (g_inLobby) {
        if (info.code == g_lobbyRoom) {
            SetStatus(L"You are already in this room");
        } else {
            MessageBoxW(g_hwnd, L"Leave the current room before joining another one.", L"JOIN ROOM",
                        MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    if (info.started) {
        SetStatus(L"The match in this room has already started");
        return;
    }
    if (info.members >= info.capacity) {
        MessageBoxW(g_hwnd, L"This room is full; please choose another one.", L"JOIN ROOM",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring password;
    if (info.hasPassword && !ShowPasswordDialog(info.code, password)) return;
    JoinRoomByCode(info.code, password);
}

void LaunchGameFlow() {
    if (g_startingGame) return;
    if (g_busy.exchange(true)) {
        AppendLog(L"Another operation is in progress; launch the game in a moment.");
        return;
    }
    g_startingGame = true;
    std::wstring server, room, name, game;
    int port = 0;
    if (!ReadSettings(server, port, room, name, game)) {
        g_startingGame = false;
        g_lobbyStarted = false;
        g_busy.store(false);
        return;
    }
    if (!FileExists(game)) {
        std::wstring message = L"The game path is not set or the file is missing; cannot start the game. Choose the game executable on the right of the main window first.";
        AppendLog(message);
        SetStatus(L"No game path set; cannot launch");
        MessageBoxW(g_hwnd, message.c_str(), L"Cannot start the game", MB_OK | MB_ICONWARNING);
        g_startingGame = false;
        g_lobbyStarted = false;
        g_busy.store(false);
        return;
    }
    if (!StartIpc()) {
        g_startingGame = false;
        g_lobbyStarted = false;
        g_busy.store(false);
        return;
    }

    SetStatus(L"Launching and injecting the game...");
    SetPeerHint(L"Launching the game; members appear once the Hook joins");
    ResetEvent(g_relayReadyEvent);
    try {
        std::wstring dll = PrepareHookFiles();
        AppendLog(L"Launching: " + game);
        std::wstring error;
        if (!LaunchAndInject(game, dll, error)) {
            AppendLog(L"Launch/inject failed: " + error);
            SetStatus(L"Injection failed");
            MessageBoxW(g_hwnd, error.c_str(), L"Injection failed", MB_OK | MB_ICONERROR);
            g_lobbyStarted = false;
        } else {
            AppendLog(L"Injection complete; the game resumed.");
            SetStatus(L"Injection complete; waiting for the Hook status...");
        }
    } catch (const std::exception& ex) {
        std::wstring error = Utf8ToWide(ex.what());
        AppendLog(L"Launch/inject failed: " + error);
        SetStatus(L"Injection failed");
        MessageBoxW(g_hwnd, error.c_str(), L"Injection failed", MB_OK | MB_ICONERROR);
        g_lobbyStarted = false;
    }
    g_startingGame = false;
    g_busy.store(false);
}

// ---------------------------------------------------------------------------
// Local single-player launch: start the game with the Hook, but do not wait
// for a relay session.  This is the "Launch game" button.
// ---------------------------------------------------------------------------
DWORD WINAPI LocalLaunchThread(LPVOID parameter) {
    std::wstring game = *reinterpret_cast<std::wstring*>(parameter);
    delete reinterpret_cast<std::wstring*>(parameter);

    std::wstring error;
    bool ok = false;
    try {
        std::wstring dll = PrepareHookFiles(true);
        ok = LaunchAndInject(game, dll, error, false);
    } catch (const std::exception& ex) {
        error = Utf8ToWide(ex.what());
    }

    if (ok) {
        AppendLog(L"The game has started.");
        SetStatus(L"The game has started.");
    } else {
        if (error.empty()) error = L"Failed to start the game.";
        AppendLog(L"Failed to launch the game: " + error);
        SetStatus(L"Failed to launch the game");
        MessageBoxW(g_hwnd, error.c_str(), L"Launch game",
                    MB_OK | MB_ICONERROR);
    }

    if (g_localGame) EnableWindow(g_localGame, TRUE);
    g_busy.store(false);
    return 0;
}

void StartLocalGameClicked() {
    if (g_busy.exchange(true)) return;

    std::wstring game = Trim(GetText(g_game));
    if (game.empty() || !FileExists(game)) {
        MessageBoxW(g_hwnd,
                    L"Set the game executable path on the right first.",
                    L"Launch game", MB_OK | MB_ICONWARNING);
        g_busy.store(false);
        return;
    }

    if (g_localGame) EnableWindow(g_localGame, FALSE);
    HANDLE thread = CreateThread(nullptr, 0, LocalLaunchThread,
                                 new std::wstring(game), 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        if (g_localGame) EnableWindow(g_localGame, TRUE);
        g_busy.store(false);
    }
}

void KickMemberClicked(int index) {
    if (!g_inLobby || !g_lobbyIsHost || g_lobbyStarted) return;
    if (index < 0 || index >= (int)g_lobbyMembers.size()) return;
    if (g_lobbyMembers[index].host) return;
    fr::LobbyKickPayload payload{};
    payload.index = (uint8_t)index;
    if (!SendLobbyPacket(fr::LobbyKick, &payload, sizeof(payload))) return;
    SetStatus(L"Kicking " + g_lobbyMembers[index].name + L" from the room...");
    AppendLog(L"Requested kick: " + g_lobbyMembers[index].name);
}

void StartGameClicked() {
    if (!g_inLobby) {
        MessageBoxW(g_hwnd, L"Create or join a room first.", L"Cannot launch", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!g_lobbyIsHost) {
        MessageBoxW(g_hwnd, L"Only the host can start the game; please wait.", L"Cannot launch",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (g_lobbyMembers.size() < 2) {
        MessageBoxW(g_hwnd, L"At least 2 players are needed to start a match.", L"Cannot start the match",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring gamePath = Trim(GetText(g_game));
    if (gamePath.empty()) {
        BrowseGame();
        gamePath = Trim(GetText(g_game));
    }
    if (!FileExists(gamePath)) {
        MessageBoxW(g_hwnd,
                    L"The game path is not set or the file is missing; cannot start the game. Choose the game executable first.",
                    L"Cannot start the game", MB_OK | MB_ICONWARNING);
        return;
    }
    for (const auto& member : g_lobbyMembers) {
        if (!member.hasGame) {
            std::wstring text = member.name + L" no game path yet, cannot launch.";
            MessageBoxW(g_hwnd, text.c_str(), L"Cannot start the game", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    SendLobbyPing();
    fr::LobbyStartPayload payload{};
    payload.mode = (uint8_t)g_lobbyMode;
    if (!SendLobbyPacket(fr::LobbyStart, &payload, sizeof(payload))) {
        SetStatus(L"Failed to send the start request");
        return;
    }
    SetStatus(L"Start request sent; waiting for all players to launch...");
    AppendLog(L"Start requested; waiting for the server to confirm.");
}

void LaunchClicked() {
    StartGameClicked();
}

void AttachClicked() {
    if (g_busy.exchange(true)) return;
    std::wstring server, room, name, game;
    int port = 0;
    if (!ReadSettings(server, port, room, name, game)) { g_busy.store(false); return; }
    if (!StartIpc()) { g_busy.store(false); return; }

    SetStatus(L"Injecting into the running game...");
    SetPeerHint(L"Note: injecting; members appear once the Hook joins the room");
    ResetEvent(g_relayReadyEvent);
    try {
        std::wstring dll = PrepareHookFiles();
        std::wstring error;
        if (!AttachAndInject(dll, error)) {
            AppendLog(L"Injection failed: " + error);
            SetStatus(L"Injection failed");
            MessageBoxW(g_hwnd, error.c_str(), L"Injection failed", MB_OK | MB_ICONERROR);
        } else {
            AppendLog(L"Injected into the game process. If the game has been running for a long time the adapter address may be cached; restart the game and inject again.");
            SetStatus(L"Injected; waiting for the Hook status...");
        }
    } catch (const std::exception& ex) {
        std::wstring error = Utf8ToWide(ex.what());
        AppendLog(L"Injection failed: " + error);
        SetStatus(L"Injection failed");
        MessageBoxW(g_hwnd, error.c_str(), L"Injection failed", MB_OK | MB_ICONERROR);
    }
    g_busy.store(false);
}

void DisconnectClicked() {
    if (g_hookReady.load()) {
        SendIpc(fr::IpcRelayStop, 0, 0, 0, 0, nullptr, 0);
        AppendLog(L"Asked the Hook to disconnect from the relay; the game is still running.");
        SetStatus(L"Disconnected (the game is still running)");
        SetPeerHint(L"Note: the relay is disconnected");
    } else {
        SetStatus(L"No Hook injected");
    }
}

void HandleLobbyEvent(const LobbyEvent& event) {
    if (event.type == fr::LobbyList) {
        if (event.payload.size() < sizeof(fr::LobbyListPayload)) return;
        const auto* payload = reinterpret_cast<const fr::LobbyListPayload*>(event.payload.data());
        int count = payload->count;
        if (count < 0) count = 0;
        if (count > fr::kMaxLobbyListRooms) count = fr::kMaxLobbyListRooms;
        std::vector<LobbyRoomInfo> list;
        list.reserve((size_t)count);
        for (int i = 0; i < count; ++i) {
            const fr::LobbyListEntry& entry = payload->rooms[i];
            LobbyRoomInfo info;
            info.code = Utf8ToWide(std::string(entry.code, strnlen(entry.code, fr::kRoomLength)));
            info.host = Utf8ToWide(std::string(entry.host, strnlen(entry.host, fr::kNameLength)));
            info.mode = (entry.mode >= 1 && entry.mode <= fr::kMaxLobbyMode) ? entry.mode : 1;
            info.members = entry.members;
            info.capacity = entry.capacity > 0 ? entry.capacity : info.mode * 2;
            info.started = (entry.flags & fr::kLobbyListStarted) != 0;
            info.hasPassword = (entry.flags & fr::kLobbyListPassword) != 0;
            if (info.code.empty()) continue;
            list.push_back(std::move(info));
        }
        std::sort(list.begin(), list.end(), [](const LobbyRoomInfo& a, const LobbyRoomInfo& b) {
            if (a.started != b.started) return !a.started;
            if (a.members != b.members) return a.members > b.members;
            return a.code < b.code;
        });
        g_roomList = std::move(list);
        int visible = 7;
        int maxScroll = (int)g_roomList.size() - visible;
        if (maxScroll < 0) maxScroll = 0;
        if (g_roomListScroll > maxScroll) g_roomListScroll = maxScroll;
        if (g_roomListScroll < 0) g_roomListScroll = 0;
        InvalidateRoomList();
        return;
    }

    if (event.type == fr::LobbyState) {
        uint8_t mode = 1;
        bool selfIsHost = false;
        std::string room;
        std::string names[fr::kMaxPeers];
        uint8_t flags[fr::kMaxPeers] = {};
        int count = 0;
        if (!fr::DecodeLobbyState(reinterpret_cast<const uint8_t*>(event.payload.data()),
                                  event.payload.size(), mode, selfIsHost, room,
                                  names, flags, count)) {
            return;
        }
        std::wstring wideRoom = Utf8ToWide(room);
        bool roomChanged = !g_inLobby || g_lobbyRoom != wideRoom;
        g_inLobby = true;
        g_lobbyWaiting = false;
        g_pendingLobbyType = 0;
        g_lobbyRoom = wideRoom;
        g_pendingRoom = wideRoom;
        g_lobbyMode = (mode >= 1 && mode <= fr::kMaxLobbyMode) ? mode : 1;
        g_lobbyIsHost = selfIsHost;
        if (roomChanged) g_lobbyStarted = false;

        g_lobbyMembers.clear();
        for (int i = 0; i < count; ++i) {
            LobbyMemberInfo info;
            info.name = Utf8ToWide(names[i]);
            info.host = (flags[i] & fr::kLobbyFlagHost) != 0;
            info.hasGame = (flags[i] & fr::kLobbyFlagHasGame) != 0;
            g_lobbyMembers.push_back(info);
        }
        OpenRoomWindow();
        UpdateLobbyUi();
        std::wstring text = L"Joined room " + g_lobbyRoom + L" (" +
                            ModeText(g_lobbyMode) + L", " +
                            std::to_wstring(g_lobbyMembers.size()) + L" players)";
        if (g_lobbyIsHost) text += L"; you are the host";
        else text += L"; waiting for the host to start";
        SetStatus(text);
        AppendLog(text);
        SendLobbyBrowse();
        return;
    }

    if (event.type == fr::LobbyStart) {
        if (!g_inLobby || g_lobbyStarted) return;
        g_lobbyStarted = true;
        UpdateLobbyUi();
        AppendLog(L"The host started the match; launching the game...");
        SetStatus(L"The host started the match; launching...");
        LaunchGameFlow();
        UpdateLobbyUi();
        return;
    }

    if (event.type == fr::LobbyChatMessage) {
        if (event.payload.size() < sizeof(fr::LobbyChatMessagePayload)) return;
        const auto* payload =
            reinterpret_cast<const fr::LobbyChatMessagePayload*>(event.payload.data());
        std::string time(payload->time, strnlen(payload->time, sizeof(payload->time)));
        std::string name(payload->name, strnlen(payload->name, sizeof(payload->name)));
        std::string text(payload->text, strnlen(payload->text, sizeof(payload->text)));
        std::wstring line = L"[" + Utf8ToWide(time) + L"] ";
        if (payload->system) {
            line += Utf8ToWide(text);
        } else {
            line += Utf8ToWide(name) + L": " + Utf8ToWide(text);
        }
        AppendRoomChatLine(line);
        return;
    }

    if (event.type == fr::Error) {
        std::wstring text = Utf8ToWide(event.payload);
        if (text.empty()) text = L"Room action failed";
        AppendLog(L"Room: " + text);
        SetStatus(text);
        bool wasWaiting = g_lobbyWaiting;
        if (wasWaiting) {
            g_lobbyWaiting = false;
            g_pendingLobbyType = 0;
        }
        bool kicked = text.find(L"Kick from room") != std::wstring::npos;
        if (kicked || text.find(L"The host left") != std::wstring::npos ||
            text.find(L"Room closed") != std::wstring::npos ||
            text.find(L"lobby session expired") != std::wstring::npos) {
            LeaveLobby(false);
        }
        if (wasWaiting || kicked) {
            MessageBoxW(g_hwnd, text.c_str(), kicked ? L"You were removed from the room" : L"Room action failed",
                        MB_OK | MB_ICONWARNING);
        }
        UpdateLobbyUi();
    }
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
HWND MakeLabel(const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   x, y, w, h, g_hwnd, nullptr, g_instance, nullptr);
    SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
    return control;
}

HWND MakeEdit(int id, const wchar_t* text, int x, int y, int w, int h, DWORD extra = 0) {
    HWND control = CreateWindowExW(0, L"EDIT", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                                   x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, g_instance, nullptr);
    SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
    return control;
}

HWND MakeButton(int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, g_instance, nullptr);
    SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
    return control;
}

void CreateControls() {
    g_lobbyControls.clear();
    g_roomControls.clear();

    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_titleFont = CreateFontW(-32, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_bannerFont = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_smallFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_inputFont = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_chatFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_roomButtonFont = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_windowBrush = CreateSolidBrush(RGB(10, 11, 11));
    g_panelBrush = CreateSolidBrush(RGB(16, 17, 17));
    g_editBrush = CreateSolidBrush(RGB(5, 6, 6));
    g_whiteBrush = CreateSolidBrush(RGB(255, 255, 255));

    // Left game list.  Only one game is selectable; the room browser is on the
    // centre panel.
    g_tree = CreateWindowExW(
        0, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_NOHSCROLL,
        12, 196, 240, 470, g_hwnd, nullptr, g_instance, nullptr);
    SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_tree, TVM_SETITEMHEIGHT, 24, 0);
    SendMessageW(g_tree, TVM_SETBKCOLOR, 0, (LPARAM)RGB(10, 11, 11));
    SendMessageW(g_tree, TVM_SETTEXTCOLOR, 0, (LPARAM)RGB(226, 190, 110));

    TVINSERTSTRUCTW insert{};
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = (LPWSTR)L"LAN games (1)";
    HTREEITEM root = TreeView_InsertItem(g_tree, &insert);

    insert.hParent = root;
    insert.item.pszText = (LPWSTR)L"Net Goal 11";
    g_treeRoomItem = TreeView_InsertItem(g_tree, &insert);

    TreeView_Expand(g_tree, root, TVE_EXPAND);
    TreeView_SelectItem(g_tree, g_treeRoomItem);

    // Centre: public room browser.  Rows are custom painted in PaintWindow so
    // each room can carry its own 加入房间 hotspot.
    g_newRoom = MakeButton(ID_NEW_ROOM, L"New Room", 632, 164, 138, 34);
    g_refreshRooms = MakeButton(ID_REFRESH_ROOMS, L"Refresh", 548, 164, 72, 34);

    // Right: player settings.
    std::wstring defaultName = DefaultNickname();
    g_name = MakeEdit(ID_NAME, defaultName.c_str(), 800, 214, 226, 30);
    g_game = MakeEdit(ID_GAME, L"", 800, 282, 226, 30);
    SendMessageW(g_game, 0x1501 /* EM_SETCUEBANNER */, FALSE,
                 (LPARAM)L"Select the game executable");
    g_browse = MakeButton(ID_BROWSE, L"Browse...", 800, 320, 108, 32);
    g_saveConfig = MakeButton(ID_SAVE_CONFIG, L"Save settings", 916, 320, 110, 32);
    g_localGame = MakeButton(ID_LOCAL_GAME, L"Launch game", 800, 357, 226, 32);

    g_verbose = CreateWindowExW(0, L"BUTTON", L"Packet log",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_AUTOCHECKBOX | BS_OWNERDRAW,
                                917, 394, 109, 22, g_hwnd, (HMENU)ID_VERBOSE, g_instance, nullptr);
    SendMessageW(g_verbose, WM_SETFONT, (WPARAM)g_smallFont, TRUE);

    g_log = CreateWindowExW(0, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                            ES_AUTOVSCROLL | ES_READONLY | ES_LEFT,
                            800, 462, 226, 186, g_hwnd, (HMENU)ID_LOG, g_instance, nullptr);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_smallFont, TRUE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
    SetupLogScrollbar();

    // Bottom status and counters.
    g_status = MakeLabel(L"Pick a room to join, or click \"New Room\" at the top right.", 20, 690, 760, 24);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_smallFont, TRUE);
    g_stats = MakeLabel(L"Up 0 / 0 KB   Down 0 / 0 KB", 800, 690, 230, 24);
    SendMessageW(g_stats, WM_SETFONT, (WPARAM)g_smallFont, TRUE);

    LoadClientConfig();
    UpdateRoomTree();
    UpdateLobbyUi();
}

void FillGradientRect(HDC dc, const RECT& rect, COLORREF top, COLORREF bottom) {
    int height = rect.bottom - rect.top;
    if (height <= 0) return;
    for (int y = 0; y < height; ++y) {
        double t = (double)y / (double)(height > 1 ? height - 1 : 1);
        int r = (int)(GetRValue(top) * (1.0 - t) + GetRValue(bottom) * t);
        int g = (int)(GetGValue(top) * (1.0 - t) + GetGValue(bottom) * t);
        int b = (int)(GetBValue(top) * (1.0 - t) + GetBValue(bottom) * t);
        HBRUSH brush = CreateSolidBrush(RGB(r, g, b));
        RECT line{rect.left, rect.top + y, rect.right, rect.top + y + 1};
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }
}

void FillRoundedGradient(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom,
                         COLORREF border, int radius) {
    HRGN region = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1,
                                     radius, radius);
    int saved = SaveDC(dc);
    SelectClipRgn(dc, region);
    FillGradientRect(dc, rc, top, bottom);
    RestoreDC(dc, saved);
    DeleteObject(region);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void FillRoundedRect(HDC dc, const RECT& rc, COLORREF fill, COLORREF border,
                     int radius);

void DrawBevelFrame(HDC dc, int x, int y, int w, int h) {
    // Gold-only rounded inset frame.  No white system border.
    RECT outer{x - 2, y - 2, x + w + 2, y + h + 2};
    FillRoundedRect(dc, outer, RGB(5, 6, 6), RGB(111, 86, 39), 8);

    HPEN dark = CreatePen(PS_SOLID, 1, RGB(69, 50, 21));
    HPEN light = CreatePen(PS_SOLID, 1, RGB(213, 173, 88));
    HGDIOBJ old = SelectObject(dc, dark);
    MoveToEx(dc, x - 1, y - 1, nullptr);
    LineTo(dc, x + w, y - 1);
    MoveToEx(dc, x - 1, y - 1, nullptr);
    LineTo(dc, x - 1, y + h);
    SelectObject(dc, light);
    MoveToEx(dc, x - 1, y + h, nullptr);
    LineTo(dc, x + w, y + h);
    MoveToEx(dc, x + w, y - 1, nullptr);
    LineTo(dc, x + w, y + h);
    SelectObject(dc, old);
    DeleteObject(light);
    DeleteObject(dark);
}

void DrawTitlePlate(HDC dc, int x, int y, int w, int h, const wchar_t* text,
                    HFONT font, bool center) {
    RECT plate{x, y, x + w, y + h};
    FillRoundedRect(dc, plate, RGB(5, 6, 6), RGB(150, 116, 51), 6);
    HPEN light = CreatePen(PS_SOLID, 1, RGB(203, 165, 82));
    HPEN dark = CreatePen(PS_SOLID, 1, RGB(55, 41, 18));
    HGDIOBJ old = SelectObject(dc, light);
    MoveToEx(dc, x + 7, y + 1, nullptr);
    LineTo(dc, x + w - 7, y + 1);
    SelectObject(dc, dark);
    MoveToEx(dc, x + 7, y + h - 2, nullptr);
    LineTo(dc, x + w - 7, y + h - 2);
    SelectObject(dc, old);
    DeleteObject(dark);
    DeleteObject(light);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 207, 129));
    SelectObject(dc, font);
    RECT textRect{x + 8, y, x + w - 8, y + h};
    DrawTextW(dc, text, -1, &textRect,
              (center ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE);
}

void DrawTitleParts(HDC dc, int x, int y, int h, const wchar_t* prefix,
                    const wchar_t* redPart, const wchar_t* suffix, HFONT font,
                    COLORREF baseColor, COLORREF redColor) {
    SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    int textY = y + (h - metrics.tmHeight) / 2;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, baseColor);
    int cursor = x;
    TextOutW(dc, cursor, textY, prefix, (int)wcslen(prefix));
    SIZE size{};
    GetTextExtentPoint32W(dc, prefix, (int)wcslen(prefix), &size);
    cursor += size.cx;
    SetTextColor(dc, redColor);
    TextOutW(dc, cursor, textY, redPart, (int)wcslen(redPart));
    GetTextExtentPoint32W(dc, redPart, (int)wcslen(redPart), &size);
    cursor += size.cx;
    SetTextColor(dc, baseColor);
    TextOutW(dc, cursor, textY, suffix, (int)wcslen(suffix));
}

void FillRoundedRect(HDC dc, const RECT& rc, COLORREF fill, COLORREF border,
                     int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawButton(const DRAWITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_BUTTON) return;
    HDC dc = item->hDC;
    RECT rc = item->rcItem;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool disabled = (item->itemState & ODS_DISABLED) != 0;
    bool tab = item->CtlID == ID_TAB_LOBBY || item->CtlID == ID_TAB_ROOM;
    bool mode = item->CtlID >= ID_MODE_11 && item->CtlID <= ID_MODE_44;
    int modeValue = 1;
    if (item->CtlID == ID_MODE_22) modeValue = 2;
    else if (item->CtlID == ID_MODE_33) modeValue = 3;
    else if (item->CtlID == ID_MODE_44) modeValue = 4;
    bool selected = (tab && ((item->CtlID == ID_TAB_LOBBY && g_activeTab == 0) ||
                             (item->CtlID == ID_TAB_ROOM && g_activeTab == 1))) ||
                    (mode && g_roomMode == modeValue);

    if (item->CtlID == ID_VERBOSE) {
        FillRect(dc, &rc, g_panelBrush);
        bool checked = g_verboseEnabled;
        RECT box{rc.left, rc.top + 3, rc.left + 15, rc.top + 18};
        FillRoundedRect(dc, box, RGB(10, 11, 10),
                        checked ? RGB(235, 197, 111) : RGB(128, 99, 43), 3);
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(238, 204, 126));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, box.left + 3, box.top + 7, nullptr);
            LineTo(dc, box.left + 6, box.top + 11);
            LineTo(dc, box.left + 12, box.top + 3);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(215, 203, 177));
        SelectObject(dc, g_smallFont);
        RECT textRect{box.right + 6, rc.top, rc.right, rc.bottom};
        DrawTextW(dc, GetText(item->hwndItem).c_str(), -1, &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    if (item->CtlID == ID_LOCAL_GAME) {
        RECT body = rc;
        FillRoundedGradient(dc, body,
                            pressed ? RGB(196, 151, 68) : RGB(226, 185, 101),
                            pressed ? RGB(126, 92, 30) : RGB(156, 116, 40),
                            RGB(255, 214, 130), 8);
        HPEN highlight = CreatePen(PS_SOLID, 1, RGB(255, 232, 170));
        HGDIOBJ oldPen = SelectObject(dc, highlight);
        MoveToEx(dc, rc.left + 12, rc.top + 3, nullptr);
        LineTo(dc, rc.right - 12, rc.top + 3);
        SelectObject(dc, oldPen);
        DeleteObject(highlight);
        HPEN shadow = CreatePen(PS_SOLID, 1, RGB(112, 81, 26));
        oldPen = SelectObject(dc, shadow);
        MoveToEx(dc, rc.left + 12, rc.bottom - 3, nullptr);
        LineTo(dc, rc.right - 12, rc.bottom - 3);
        SelectObject(dc, oldPen);
        DeleteObject(shadow);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(45, 33, 12));
        SelectObject(dc, g_font);
        RECT textRect = rc;
        if (pressed) OffsetRect(&textRect, 0, 1);
        DrawTextW(dc, GetText(item->hwndItem).c_str(), -1, &textRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    COLORREF background;
    COLORREF border;
    COLORREF text;
    if (tab || mode) {
        background = selected ? RGB(44, 18, 16) : RGB(24, 25, 23);
        border = selected ? RGB(218, 92, 76) : RGB(91, 77, 46);
        text = selected ? RGB(255, 225, 205) : RGB(186, 178, 158);
    } else if (disabled) {
        background = RGB(58, 38, 36);
        border = RGB(105, 72, 68);
        text = RGB(145, 128, 124);
    } else {
        background = pressed ? RGB(132, 18, 16) : RGB(202, 48, 40);
        border = pressed ? RGB(255, 155, 135) : RGB(255, 128, 108);
        text = RGB(255, 246, 240);
    }

    if (!tab && !mode && !disabled) {
        RECT body = rc;
        FillRoundedGradient(dc, body,
                            pressed ? RGB(165, 26, 22) : RGB(224, 64, 52),
                            pressed ? RGB(95, 5, 5) : RGB(142, 10, 10),
                            border, 8);
        HPEN highlight = CreatePen(PS_SOLID, 1, RGB(255, 150, 128));
        HGDIOBJ oldPen = SelectObject(dc, highlight);
        MoveToEx(dc, rc.left + 12, rc.top + 3, nullptr);
        LineTo(dc, rc.right - 12, rc.top + 3);
        SelectObject(dc, oldPen);
        DeleteObject(highlight);
        HPEN shadow = CreatePen(PS_SOLID, 1, RGB(92, 0, 0));
        oldPen = SelectObject(dc, shadow);
        MoveToEx(dc, rc.left + 12, rc.bottom - 3, nullptr);
        LineTo(dc, rc.right - 12, rc.bottom - 3);
        SelectObject(dc, oldPen);
        DeleteObject(shadow);
    } else {
        FillRoundedRect(dc, rc, background, border, (tab || mode) ? 4 : 8);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    SelectObject(dc, g_font);
    RECT textRect = rc;
    if (pressed) OffsetRect(&textRect, 0, 1);
    DrawTextW(dc, GetText(item->hwndItem).c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

int RoomRowAt(POINT pt) {
    if (pt.x < kRoomListLeft || pt.x >= kRoomListRight) return -1;
    if (pt.y < kRoomListTop) return -1;
    int row = (pt.y - kRoomListTop) / kRoomRowHeight;
    if (row < 0 || row >= kRoomListVisible) return -1;
    int index = g_roomListScroll + row;
    if (index < 0 || index >= static_cast<int>(g_roomList.size())) return -1;
    return index;
}

RECT RoomJoinRect(int index) {
    int row = index - g_roomListScroll;
    int top = kRoomListTop + row * kRoomRowHeight;
    RECT rc{kRoomListRight - 104, top + 17, kRoomListRight - 12, top + 55};
    return rc;
}

bool PointInRect(const RECT& rc, POINT pt) {
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

void PaintRoomList(HDC dc) {
    SelectObject(dc, g_smallFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(178, 166, 140));
    TextOutW(dc, kRoomListLeft + 10, 204, L"ROOM / HOST", (int)wcslen(L"ROOM / HOST"));
    TextOutW(dc, kRoomListLeft + 250, 204, L"Mode", (int)wcslen(L"Mode"));
    TextOutW(dc, kRoomListLeft + 315, 204, L"Players", (int)wcslen(L"Players"));

    int listBottom = kRoomListTop + kRoomRowHeight * kRoomListVisible;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, kRoomListLeft, kRoomListTop - 2, kRoomListRight, listBottom + 2);

    if (g_roomList.empty()) {
        SelectObject(dc, g_font);
        SetTextColor(dc, RGB(150, 139, 118));
        RECT empty{kRoomListLeft, kRoomListTop, kRoomListRight,
                   kRoomListTop + kRoomRowHeight * 2};
        DrawTextW(dc,
                  g_lobbyWaiting ? L"Connecting to the room server..." : L"No rooms yet; click \"New Room\" at the top right",
                  -1, &empty, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RestoreDC(dc, saved);
        return;
    }

    int total = static_cast<int>(g_roomList.size());
    for (int row = 0; row < kRoomListVisible; ++row) {
        int index = g_roomListScroll + row;
        if (index >= total) break;
        const LobbyRoomInfo& info = g_roomList[index];
        int top = kRoomListTop + row * kRoomRowHeight;
        RECT rc{kRoomListLeft, top, kRoomListRight, top + kRoomRowHeight - 5};
        bool hover = index == g_hoverRoomRow;
        FillRoundedRect(dc, rc, hover ? RGB(31, 29, 21) : RGB(21, 22, 21),
                        hover ? RGB(188, 148, 68) : RGB(70, 61, 38), 8);

        RECT codeRect{rc.left + 16, rc.top + 4, rc.left + 244, rc.top + 38};
        SelectObject(dc, g_bannerFont);
        SetTextColor(dc, RGB(243, 213, 143));
        DrawTextW(dc, info.code.c_str(), -1, &codeRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        std::wstring sub = info.host.empty() ? L"Host" : (L"Host " + info.host);
        if (info.hasPassword) sub += L"   ·   locked";
        if (info.started) sub += L"   ·   started";
        RECT hostRect{rc.left + 16, rc.top + 38, rc.left + 244, rc.top + 62};
        SelectObject(dc, g_smallFont);
        SetTextColor(dc, RGB(165, 155, 134));
        DrawTextW(dc, sub.c_str(), -1, &hostRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(dc, g_font);
        SetTextColor(dc, RGB(224, 203, 157));
        TextOutW(dc, rc.left + 250, rc.top + 25, ModeText(info.mode).c_str(), 3);

        std::wstring count = std::to_wstring(info.members) + L"/" +
                             std::to_wstring(info.capacity);
        SetTextColor(dc, info.members >= info.capacity ? RGB(219, 120, 108)
                                                       : RGB(208, 197, 173));
        TextOutW(dc, rc.left + 315, rc.top + 25, count.c_str(),
                 static_cast<int>(count.size()));

        RECT join = RoomJoinRect(index);
        bool joinHover = hover && g_hoverJoinButton;
        bool currentRoom = g_inLobby && info.code == g_lobbyRoom;
        if (currentRoom) {
            FillRoundedRect(dc, join, RGB(38, 38, 35), RGB(88, 79, 54), 6);
            SetTextColor(dc, RGB(170, 155, 122));
            DrawTextW(dc, L"Joined", -1, &join,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (info.started) {
            FillRoundedRect(dc, join, RGB(38, 38, 35), RGB(88, 79, 54), 6);
            SetTextColor(dc, RGB(135, 126, 108));
            DrawTextW(dc, L"Started", -1, &join,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            FillRoundedGradient(dc, join,
                                joinHover ? RGB(236, 78, 62) : RGB(216, 56, 46),
                                joinHover ? RGB(160, 14, 12) : RGB(140, 10, 10),
                                RGB(255, 130, 110), 6);
            HPEN highlight = CreatePen(PS_SOLID, 1, RGB(255, 160, 138));
            HGDIOBJ oldPen = SelectObject(dc, highlight);
            MoveToEx(dc, join.left + 8, join.top + 2, nullptr);
            LineTo(dc, join.right - 8, join.top + 2);
            SelectObject(dc, oldPen);
            DeleteObject(highlight);
            SetTextColor(dc, RGB(255, 246, 240));
            DrawTextW(dc, L"JOIN ROOM", -1, &join,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    RestoreDC(dc, saved);

    if (total > kRoomListVisible) {
        SelectObject(dc, g_smallFont);
        SetTextColor(dc, RGB(140, 129, 106));
        wchar_t hint[64];
        swprintf(hint, 64, L"%d rooms · scroll to see more", total);
        TextOutW(dc, kRoomListLeft + 4, listBottom + 2, hint, (int)wcslen(hint));
    }
}

// ---------------------------------------------------------------------------
// Separate room window
// ---------------------------------------------------------------------------
constexpr int kRoomWndWidth = 900;
constexpr int kRoomWndHeight = 700;
constexpr int kRoomTitleHeight = 52;
constexpr int kRoomRowTop = 168;
constexpr int kRoomMemberRowHeight = 42;
constexpr int kRoomStartTop = 600;
constexpr int kMaxRoomMembers = 10;
constexpr int kRoomMemberLeft = 614;
constexpr int kRoomMemberRight = 884;
constexpr int kRoomChatLeft = 28;
constexpr int kRoomChatRight = 584;
constexpr int kRoomChatTop = 170;
constexpr int kRoomChatLogHeight = 400;
constexpr int kRoomChatInputTop = 580;

RECT RoomStartRect() {
    return RECT{kRoomMemberLeft, kRoomStartTop, kRoomMemberLeft + 130, kRoomStartTop + 48};
}

RECT RoomLeaveRect() {
    return RECT{kRoomMemberLeft + 140, kRoomStartTop, kRoomMemberRight, kRoomStartTop + 48};
}

RECT RoomKickRect(int index) {
    int top = kRoomRowTop + index * kRoomMemberRowHeight;
    return RECT{810, top + 6, 872, top + 38};
}

void DrawRoomButton(HDC dc, const RECT& rc, const std::wstring& text, bool enabled,
                    bool primary) {
    COLORREF textColor;
    if (primary && enabled) {
        FillRoundedGradient(dc, rc, RGB(224, 64, 52), RGB(142, 10, 10),
                            RGB(255, 128, 108), 8);
        textColor = RGB(255, 246, 240);
    } else if (primary) {
        FillRoundedRect(dc, rc, RGB(58, 38, 36), RGB(105, 72, 68), 8);
        textColor = RGB(145, 128, 124);
    } else {
        FillRoundedGradient(dc, rc, RGB(112, 28, 24), RGB(70, 8, 8),
                            RGB(190, 80, 68), 8);
        textColor = RGB(255, 235, 225);
    }
    if (enabled) {
        HPEN highlight = CreatePen(PS_SOLID, 1, RGB(255, 150, 128));
        HGDIOBJ oldPen = SelectObject(dc, highlight);
        MoveToEx(dc, rc.left + 12, rc.top + 3, nullptr);
        LineTo(dc, rc.right - 12, rc.top + 3);
        SelectObject(dc, oldPen);
        DeleteObject(highlight);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor);
    SelectObject(dc, g_roomButtonFont);
    RECT textRect = rc;
    DrawTextW(dc, text.c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void PaintRoomWindow(HWND hwnd, HDC dc) {
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, g_windowBrush);

    RECT title{0, 0, kRoomWndWidth, kRoomTitleHeight};
    FillGradientRect(dc, title, RGB(218, 58, 48), RGB(118, 8, 8));
    HBRUSH titleLine = CreateSolidBrush(RGB(255, 140, 120));
    RECT titleTop{0, 2, kRoomWndWidth, 3};
    FillRect(dc, &titleTop, titleLine);
    DeleteObject(titleLine);
    HBRUSH titleShadow = CreateSolidBrush(RGB(88, 0, 0));
    RECT titleBottom{0, kRoomTitleHeight - 1, kRoomWndWidth, kRoomTitleHeight};
    FillRect(dc, &titleBottom, titleShadow);
    DeleteObject(titleShadow);

    SetBkMode(dc, TRANSPARENT);
    DrawTitleParts(dc, 18, 0, kRoomTitleHeight, L"Game room  ", g_lobbyRoom.c_str(), L"",
                   g_bannerFont, RGB(255, 245, 238), RGB(255, 210, 140));

    HPEN closePen = CreatePen(PS_SOLID, 2, RGB(255, 235, 225));
    HGDIOBJ oldPen = SelectObject(dc, closePen);
    int cx = kRoomWndWidth - 26;
    int cy = kRoomTitleHeight / 2;
    MoveToEx(dc, cx - 7, cy - 7, nullptr);
    LineTo(dc, cx + 7, cy + 7);
    MoveToEx(dc, cx + 7, cy - 7, nullptr);
    LineTo(dc, cx - 7, cy + 7);
    SelectObject(dc, oldPen);
    DeleteObject(closePen);

    // Minimize dash, spaced like the main window's caption buttons.
    HPEN minPen = CreatePen(PS_SOLID, 2, RGB(255, 235, 225));
    oldPen = SelectObject(dc, minPen);
    int mx = kRoomWndWidth - 72;
    MoveToEx(dc, mx - 7, cy, nullptr);
    LineTo(dc, mx + 7, cy);
    SelectObject(dc, oldPen);
    DeleteObject(minPen);

    // Room summary.
    RECT info{14, 60, kRoomWndWidth - 14, 136};
    FillRoundedRect(dc, info, RGB(22, 23, 22), RGB(88, 75, 45), 10);
    SetTextColor(dc, RGB(210, 190, 150));
    SelectObject(dc, g_font);
    TextOutW(dc, 28, 66, L"Room code", (int)wcslen(L"Room code"));
    TextOutW(dc, 300, 66, L"Mode", (int)wcslen(L"Mode"));
    TextOutW(dc, 380, 66, L"Players", (int)wcslen(L"Players"));
    TextOutW(dc, 470, 66, L"Password", (int)wcslen(L"Password"));

    SetTextColor(dc, RGB(243, 213, 143));
    SelectObject(dc, g_bannerFont);
    TextOutW(dc, 28, 88, g_lobbyRoom.c_str(), static_cast<int>(g_lobbyRoom.size()));

    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(224, 203, 157));
    std::wstring mode = ModeText(g_lobbyMode);
    TextOutW(dc, 300, 90, mode.c_str(), static_cast<int>(mode.size()));
    std::wstring count = std::to_wstring(g_lobbyMembers.size()) + L"/" +
                         std::to_wstring(g_lobbyMode * 2);
    TextOutW(dc, 380, 90, count.c_str(), static_cast<int>(count.size()));
    {
        const wchar_t* passwordText = g_lobbyHasPassword ? L"Yes" : L"No";
        TextOutW(dc, 470, 90, passwordText, (int)wcslen(passwordText));
    }

    // Member list.
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(226, 190, 110));
    TextOutW(dc, kRoomMemberLeft + 6, 144, L"ROOM MEMBERS",
             (int)wcslen(L"ROOM MEMBERS"));

    // Chat panel on the left.  The edit controls themselves are child
    // windows; this only draws the frame, title and divider.
    RECT chatPanel{16, 140, 596, 676};
    FillRoundedRect(dc, chatPanel, RGB(18, 19, 18), RGB(70, 60, 38), 10);
    SetTextColor(dc, RGB(226, 190, 110));
    TextOutW(dc, kRoomChatLeft, 142, L"ROOM CHAT", (int)wcslen(L"ROOM CHAT"));

    RECT logFrame{kRoomChatLeft - 4, kRoomChatTop - 4, kRoomChatRight + 4,
                  kRoomChatTop + kRoomChatLogHeight + 4};
    FillRoundedRect(dc, logFrame, RGB(8, 9, 9), RGB(158, 126, 64), 8);
    RECT inputFrame{kRoomChatLeft - 4, kRoomChatInputTop - 4,
                    kRoomChatRight - 70, kRoomChatInputTop + 48};
    FillRoundedRect(dc, inputFrame, RGB(8, 9, 9), RGB(158, 126, 64), 8);

    for (int i = 0; i < static_cast<int>(g_lobbyMembers.size()) && i < kMaxRoomMembers; ++i) {
        const LobbyMemberInfo& member = g_lobbyMembers[i];
        int top = kRoomRowTop + i * kRoomMemberRowHeight;
        RECT row{kRoomMemberLeft, top, kRoomMemberRight, top + kRoomMemberRowHeight - 6};
        bool hover = i == g_roomHoverKick;
        FillRoundedRect(dc, row, hover ? RGB(30, 28, 21) : RGB(21, 22, 21),
                        hover ? RGB(150, 120, 60) : RGB(62, 55, 36), 8);

        SelectObject(dc, g_font);
        SetTextColor(dc, RGB(230, 220, 196));
        std::wstring name = member.name;
        if (member.host) name += L"   [host]";
        TextOutW(dc, row.left + 14, row.top + 9, name.c_str(),
                 static_cast<int>(name.size()));

        SelectObject(dc, g_smallFont);
        SetTextColor(dc, member.hasGame ? RGB(150, 190, 130) : RGB(205, 120, 110));
        const wchar_t* gameState = member.hasGame ? L"Ready" : L"Not set";
        TextOutW(dc, kRoomMemberLeft + 146, row.top + 11, gameState, (int)wcslen(gameState));

        if (g_lobbyIsHost && !member.host) {
            RECT kick = RoomKickRect(i);
            bool kickHover = hover;
            FillRoundedRect(dc, kick, kickHover ? RGB(146, 60, 48) : RGB(96, 44, 37),
                            RGB(196, 96, 78), 6);
            SetTextColor(dc, RGB(246, 220, 210));
            SelectObject(dc, g_smallFont);
            DrawTextW(dc, L"KICK", -1, &kick,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    bool canStart = g_lobbyIsHost && !g_lobbyStarted;
    SelectObject(dc, g_font);
    DrawRoomButton(dc, RoomStartRect(), L"START", canStart, true);
    DrawRoomButton(dc, RoomLeaveRect(), L"LEAVE", true, false);

    HBRUSH outer = CreateSolidBrush(RGB(155, 121, 55));
    FrameRect(dc, &client, outer);
    DeleteObject(outer);
}

LRESULT CALLBACK RoomChatEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KEYDOWN && wParam == VK_RETURN) {
        PostMessageW(GetParent(hwnd), WM_APP + 1, 0, 0);
        return 0;
    }
    if (message == WM_CHAR && (wParam == L'\r' || wParam == L'\n')) return 0;
    if (g_roomChatEditOldProc) {
        return CallWindowProcW(g_roomChatEditOldProc, hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void AppendRoomChatLine(const std::wstring& line) {
    g_roomChatLines.push_back(line);
    if (g_roomChatLines.size() > 200) g_roomChatLines.erase(g_roomChatLines.begin());
    if (!g_roomChatLog || !IsWindow(g_roomChatLog)) return;
    std::wstring all;
    for (const auto& item : g_roomChatLines) {
        all += item;
        all += L"\r\n";
    }
    SetWindowTextW(g_roomChatLog, all.c_str());
    int lines = (int)SendMessageW(g_roomChatLog, EM_GETLINECOUNT, 0, 0);
    SendMessageW(g_roomChatLog, EM_LINESCROLL, 0, lines);
}

void ResetRoomChat() {
    g_roomChatLines.clear();
    if (g_roomChatLog && IsWindow(g_roomChatLog)) SetWindowTextW(g_roomChatLog, L"");
}

void SendRoomChat() {
    if (!g_inLobby || !g_roomChatInput || !IsWindow(g_roomChatInput)) return;
    std::wstring text = Trim(GetText(g_roomChatInput));
    if (text.empty()) return;
    std::string utf8 = TruncateUtf8(WideToUtf8(text), fr::kLobbyChatTextLength - 1);
    fr::LobbyChatPayload payload{};
    memcpy(payload.text, utf8.data(), utf8.size());
    if (!SendLobbyPacket(fr::LobbyChat, &payload, sizeof(payload))) {
        SetStatus(L"Chat send failed");
        return;
    }
    SetText(g_roomChatInput, L"");
    SetFocus(g_roomChatInput);
}

void CreateRoomChatControls(HWND hwnd) {
    g_roomChatLog = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
            ES_NOHIDESEL,
        kRoomChatLeft, kRoomChatTop, kRoomChatRight - kRoomChatLeft,
        kRoomChatLogHeight, hwnd, (HMENU)ID_ROOM_CHAT_LOG, g_instance, nullptr);
    g_roomChatInput = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        kRoomChatLeft, kRoomChatInputTop, 482, 44, hwnd,
        (HMENU)ID_ROOM_CHAT_INPUT, g_instance, nullptr);
    g_roomChatSend = CreateWindowExW(
        0, L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        kRoomChatRight - 66, kRoomChatInputTop, 66, 44, hwnd,
        (HMENU)ID_ROOM_CHAT_SEND, g_instance, nullptr);
    if (!g_roomChatLog || !g_roomChatInput || !g_roomChatSend) return;
    SendMessageW(g_roomChatLog, WM_SETFONT, (WPARAM)g_chatFont, TRUE);
    SendMessageW(g_roomChatInput, WM_SETFONT, (WPARAM)g_inputFont, TRUE);
    SendMessageW(g_roomChatSend, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_roomChatInput, EM_SETLIMITTEXT, 200, 0);
    SendMessageW(g_roomChatInput, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(10, 10));
    g_roomChatEditOldProc = (WNDPROC)SetWindowLongPtrW(
        g_roomChatInput, GWLP_WNDPROC, (LONG_PTR)RoomChatEditProc);
    if (!g_roomChatLines.empty()) {
        std::wstring all;
        for (const auto& item : g_roomChatLines) {
            all += item;
            all += L"\r\n";
        }
        SetWindowTextW(g_roomChatLog, all.c_str());
        int lines = (int)SendMessageW(g_roomChatLog, EM_GETLINECOUNT, 0, 0);
        SendMessageW(g_roomChatLog, EM_LINESCROLL, 0, lines);
    }
}

// WS_EX_COMPOSITED windows can come back from the taskbar without a
// WM_PAINT for the whole client area: Windows keeps the stale (or empty)
// composition surface for the part that has no child controls, which is
// where the custom title bar and the banner live.  Force a full repaint
// whenever a window is restored or activated.
void RepaintWholeWindow(HWND hwnd) {
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

LRESULT CALLBACK RoomWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE:
            if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED)
                RepaintWholeWindow(hwnd);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) RepaintWholeWindow(hwnd);
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            PaintRoomWindow(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_ROOM_CHAT_SEND && HIWORD(wParam) == BN_CLICKED) {
                SendRoomChat();
                return 0;
            }
            break;
        case WM_APP + 1:
            SendRoomChat();
            return 0;
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!item || item->CtlID != ID_ROOM_CHAT_SEND) break;
            DrawRoomButton(item->hDC, item->rcItem, L"Send", true, true);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(230, 220, 200));
            SetBkColor(dc, RGB(5, 6, 6));
            return (LRESULT)g_editBrush;
        }
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(230, 220, 200));
            return (LRESULT)g_windowBrush;
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            if (pt.y >= 0 && pt.y < kRoomTitleHeight) {
                if (pt.x >= kRoomWndWidth - 92 && pt.x < kRoomWndWidth - 52) {
                    return HTCLIENT;
                }
                if (pt.x < kRoomWndWidth - 52) return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int kick = -1;
            if (g_lobbyIsHost) {
                for (int i = 0; i < static_cast<int>(g_lobbyMembers.size()); ++i) {
                    if (!g_lobbyMembers[i].host && PointInRect(RoomKickRect(i), pt)) {
                        kick = i;
                        break;
                    }
                }
            }
            bool startHover = PointInRect(RoomStartRect(), pt);
            bool leaveHover = PointInRect(RoomLeaveRect(), pt);
            if (kick != g_roomHoverKick || startHover != g_roomHoverStart ||
                leaveHover != g_roomHoverLeave) {
                g_roomHoverKick = kick;
                g_roomHoverStart = startHover;
                g_roomHoverLeave = leaveHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_roomHoverKick != -1 || g_roomHoverStart || g_roomHoverLeave) {
                g_roomHoverKick = -1;
                g_roomHoverStart = false;
                g_roomHoverLeave = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.y >= 0 && pt.y < kRoomTitleHeight &&
                pt.x >= kRoomWndWidth - 92 && pt.x < kRoomWndWidth - 52) {
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            }
            if (pt.y >= 0 && pt.y < kRoomTitleHeight && pt.x >= kRoomWndWidth - 52) {
                LeaveLobby(true);
                return 0;
            }
            if (PointInRect(RoomStartRect(), pt)) {
                if (!g_lobbyIsHost) {
                    MessageBoxW(hwnd, L"Only the host can start the match; please wait.",
                                L"START MATCH", MB_OK | MB_ICONINFORMATION);
                } else if (g_lobbyStarted) {
                    MessageBoxW(hwnd, L"The match has already started.", L"START MATCH",
                                MB_OK | MB_ICONINFORMATION);
                } else {
                    StartGameClicked();
                }
                return 0;
            }
            if (PointInRect(RoomLeaveRect(), pt)) {
                LeaveLobby(true);
                return 0;
            }
            if (g_lobbyIsHost) {
                for (int i = 0; i < static_cast<int>(g_lobbyMembers.size()); ++i) {
                    if (!g_lobbyMembers[i].host && PointInRect(RoomKickRect(i), pt)) {
                        KickMemberClicked(i);
                        return 0;
                    }
                }
            }
            return 0;
        }
        case WM_CLOSE:
            LeaveLobby(true);
            return 0;
        case WM_DESTROY:
            if (g_roomWindow == hwnd) g_roomWindow = nullptr;
            g_roomChatLog = nullptr;
            g_roomChatInput = nullptr;
            g_roomChatSend = nullptr;
            g_roomChatEditOldProc = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void EnsureRoomWindowClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = RoomWindowProc;
    cls.hInstance = g_instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = g_windowBrush;
    cls.lpszClassName = L"NetGoal11Room";
    cls.hIcon = g_iconLarge ? g_iconLarge : LoadIconW(nullptr, IDI_APPLICATION);
    cls.hIconSm = g_iconSmall;
    RegisterClassExW(&cls);
    registered = true;
}

void OpenRoomWindow() {
    if (g_roomWindow && IsWindow(g_roomWindow)) {
        RefreshRoomWindow();
        return;
    }
    EnsureRoomWindowClass();
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - kRoomWndWidth) / 2;
    int y = work.top + ((work.bottom - work.top) - kRoomWndHeight) / 2;
    g_roomWindow = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_COMPOSITED,
                                   L"NetGoal11Room",
                                   L"Net Goal 11 - Game room", WS_POPUP | WS_CLIPCHILDREN,
                                   x, y, kRoomWndWidth, kRoomWndHeight, g_hwnd, nullptr,
                                   g_instance, nullptr);
    if (!g_roomWindow) return;
    CreateRoomChatControls(g_roomWindow);
    ShowWindow(g_roomWindow, SW_SHOW);
    UpdateWindow(g_roomWindow);
    if (g_roomChatInput && IsWindow(g_roomChatInput)) SetFocus(g_roomChatInput);
}

void CloseRoomWindow() {
    if (!g_roomWindow) return;
    HWND window = g_roomWindow;
    g_roomWindow = nullptr;
    if (IsWindow(window)) DestroyWindow(window);
}

void RefreshRoomWindow() {
    if (g_roomWindow && IsWindow(g_roomWindow)) {
        InvalidateRect(g_roomWindow, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// Modal dialogs (create room / room password)
// ---------------------------------------------------------------------------
std::wstring SuggestRoomCode() {
    return DefaultRoomName();
}

BOOL CALLBACK StyleDialogChildProc(HWND child, LPARAM) {
    wchar_t className[16] = {};
    GetClassNameW(child, className, 16);
    HFONT font = _wcsicmp(className, L"Edit") == 0 ? g_inputFont : g_font;
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

void StyleDialogControls(HWND dialog) {
    EnumChildWindows(dialog, StyleDialogChildProc, 0);
}

LRESULT CALLBACK RoomDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<RoomDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == ID_DLG_OK && state) {
                state->room = Trim(GetText(state->editRoom));
                state->password = GetText(state->editPassword);
                if (state->kind == 0) {
                    if (state->room.empty()) {
                        MessageBoxW(hwnd, L"Enter a room code.", L"New Room",
                                    MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    if (state->mode < 1 || state->mode > fr::kMaxLobbyMode) state->mode = 1;
                }
                state->ok = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == ID_DLG_CANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (state && state->kind == 0) {
                static const int modeIds[5] = {ID_DLG_MODE_11, ID_DLG_MODE_22,
                                               ID_DLG_MODE_33, ID_DLG_MODE_44,
                                               ID_DLG_MODE_55};
                for (int i = 0; i < 5; ++i) {
                    if (id == modeIds[i]) {
                        state->mode = i + 1;
                        for (int j = 0; j < 5; ++j) {
                            if (state->radios[j]) InvalidateRect(state->radios[j], nullptr, TRUE);
                        }
                        return 0;
                    }
                }
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!state || !dis) break;
            if (dis->CtlID == ID_DLG_OK) {
                DrawRoomButton(dis->hDC, dis->rcItem, GetText(dis->hwndItem), true, true);
                return TRUE;
            }
            if (dis->CtlID == ID_DLG_CANCEL) {
                DrawRoomButton(dis->hDC, dis->rcItem, GetText(dis->hwndItem), true, false);
                return TRUE;
            }
            int index = -1;
            for (int i = 0; i < 5; ++i) {
                if ((HWND)dis->hwndItem == state->radios[i]) index = i;
            }
            if (index < 0) break;
            static const wchar_t* modeTexts[5] = {L"1v1", L"2v2", L"3v3", L"4v4", L"5v5"};
            HDC dc = dis->hDC;
            RECT rc = dis->rcItem;
            FillRect(dc, &rc, g_windowBrush);
            bool checked = (state->mode == index + 1);
            int cy = (rc.top + rc.bottom) / 2;
            int cx = rc.left + 10;
            SetDCBrushColor(dc, checked ? RGB(207, 161, 68) : RGB(10, 11, 11));
            SetDCPenColor(dc, checked ? RGB(240, 214, 150) : RGB(150, 139, 118));
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
            Ellipse(dc, cx - 7, cy - 7, cx + 7, cy + 7);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(235, 228, 211));
            SelectObject(dc, g_font);
            RECT textRc{rc.left + 22, rc.top, rc.right, rc.bottom};
            DrawTextW(dc, modeTexts[index], -1, &textRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (dis->itemState & ODS_FOCUS) DrawFocusRect(dc, &rc);
            return TRUE;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect((HDC)wParam, &client, g_windowBrush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, g_windowBrush);
            if (state) {
                if (state->kind == 0) {
                    RECT roomFrame{146, 20, 536, 68};
                    RECT passwordFrame{146, 82, 536, 130};
                    FillRoundedRect(dc, roomFrame, RGB(8, 9, 9), RGB(158, 126, 64), 8);
                    FillRoundedRect(dc, passwordFrame, RGB(8, 9, 9), RGB(158, 126, 64), 8);
                } else {
                    RECT passwordFrame{16, 50, 364, 98};
                    FillRoundedRect(dc, passwordFrame, RGB(8, 9, 9), RGB(158, 126, 64), 8);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(10, 11, 11));
            SetTextColor(dc, RGB(210, 200, 178));
            return (LRESULT)g_windowBrush;
        }
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(210, 200, 178));
            return (LRESULT)g_windowBrush;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(235, 228, 211));
            SetBkColor(dc, RGB(5, 6, 6));
            return (LRESULT)g_editBrush;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void EnsureRoomDialogClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = RoomDialogProc;
    cls.hInstance = g_instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = g_windowBrush;
    cls.lpszClassName = L"NetGoal11RoomDialog";
    cls.hIcon = g_iconLarge ? g_iconLarge : LoadIconW(nullptr, IDI_APPLICATION);
    cls.hIconSm = g_iconSmall;
    RegisterClassExW(&cls);
    registered = true;
}

bool RunRoomDialog(HWND dialog, HWND owner, RoomDialogState& state) {
    SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(dialog);
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            PostMessageW(dialog, WM_COMMAND, ID_DLG_OK, 0);
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(dialog);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return state.ok;
}

bool ShowCreateRoomDialog(std::wstring& room, std::wstring& password, int& mode) {
    EnsureRoomDialogClass();
    RoomDialogState state;
    state.kind = 0;
    state.mode = mode;
    if (state.mode < 1 || state.mode > fr::kMaxLobbyMode) state.mode = 1;
    RECT rc{0, 0, 560, 330};
    AdjustWindowRect(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NetGoal11RoomDialog",
                                  L"New Room", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                  rc.bottom - rc.top, g_hwnd, nullptr, g_instance, nullptr);
    if (!dialog) return false;

    HWND label1 = CreateWindowExW(0, L"STATIC", L"Room code", WS_CHILD | WS_VISIBLE,
                                  28, 30, 110, 24, dialog, nullptr, g_instance, nullptr);
    state.editRoom = CreateWindowExW(0, L"EDIT", SuggestRoomCode().c_str(),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     150, 24, 382, 40, dialog, nullptr, g_instance, nullptr);
    HWND label2 = CreateWindowExW(0, L"STATIC", L"Password", WS_CHILD | WS_VISIBLE,
                                  28, 92, 110, 24, dialog, nullptr, g_instance, nullptr);
    state.editPassword = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                             ES_PASSWORD | ES_AUTOHSCROLL,
                                         150, 86, 382, 40, dialog, nullptr, g_instance, nullptr);
    SendMessageW(state.editPassword, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional");
    HWND label3 = CreateWindowExW(0, L"STATIC", L"Match mode", WS_CHILD | WS_VISIBLE,
                                  28, 154, 110, 24, dialog, nullptr, g_instance, nullptr);

    const wchar_t* modeTexts[5] = {L"1v1", L"2v2", L"3v3", L"4v4", L"5v5"};
    const int modeIds[5] = {ID_DLG_MODE_11, ID_DLG_MODE_22, ID_DLG_MODE_33,
                            ID_DLG_MODE_44, ID_DLG_MODE_55};
    for (int i = 0; i < 5; ++i) {
        state.radios[i] = CreateWindowExW(0, L"BUTTON", modeTexts[i],
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                              BS_OWNERDRAW | (i == 0 ? WS_GROUP : 0),
                                          150 + i * 76, 150, 70, 26, dialog,
                                          (HMENU)(INT_PTR)modeIds[i], g_instance, nullptr);
    }

    HWND hint = CreateWindowExW(0, L"STATIC",
                                L"You enter the room automatically; share the room code with the other players.",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                28, 198, 504, 40, dialog, nullptr, g_instance, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"Create", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  BS_OWNERDRAW,
                              170, 252, 100, 42, dialog, (HMENU)ID_DLG_OK, g_instance, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      BS_OWNERDRAW,
                                  290, 252, 100, 42, dialog, (HMENU)ID_DLG_CANCEL,
                                  g_instance, nullptr);
    SendMessageW(state.editRoom, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(10, 10));
    SendMessageW(state.editPassword, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(10, 10));
    (void)label1; (void)label2; (void)label3; (void)hint; (void)ok; (void)cancel;
    StyleDialogControls(dialog);

    bool okResult = RunRoomDialog(dialog, g_hwnd, state);
    if (!okResult) return false;
    room = state.room;
    password = state.password;
    mode = state.mode;
    return true;
}

bool ShowPasswordDialog(const std::wstring& room, std::wstring& password) {
    EnsureRoomDialogClass();
    RoomDialogState state;
    state.kind = 1;
    state.room = room;
    RECT rc{0, 0, 380, 190};
    AdjustWindowRect(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NetGoal11RoomDialog",
                                  L"JOIN ROOM", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                  rc.bottom - rc.top, g_hwnd, nullptr, g_instance, nullptr);
    if (!dialog) return false;

    std::wstring labelText = L"Room " + room + L" requires a password";
    HWND label = CreateWindowExW(0, L"STATIC", labelText.c_str(), WS_CHILD | WS_VISIBLE,
                                 24, 22, 330, 24, dialog, nullptr, g_instance, nullptr);
    state.editPassword = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                             ES_PASSWORD | ES_AUTOHSCROLL,
                                         20, 54, 340, 40, dialog, nullptr, g_instance, nullptr);
    SendMessageW(state.editPassword, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(10, 10));
    HWND ok = CreateWindowExW(0, L"BUTTON", L"Join", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  BS_OWNERDRAW,
                              90, 120, 90, 40, dialog, (HMENU)ID_DLG_OK, g_instance, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      BS_OWNERDRAW,
                                  200, 120, 90, 40, dialog, (HMENU)ID_DLG_CANCEL,
                                  g_instance, nullptr);
    (void)label; (void)ok; (void)cancel;
    StyleDialogControls(dialog);
    SetFocus(state.editPassword);

    bool okResult = RunRoomDialog(dialog, g_hwnd, state);
    if (!okResult) return false;
    password = state.password;
    return true;
}

void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    FillRect(dc, &client, g_windowBrush);

    // Custom gold title bar, sized larger than the normal Windows caption.
    RECT titleBar{0, 0, kClientWidth, kTitleBarHeight};
    FillGradientRect(dc, titleBar, RGB(218, 58, 48), RGB(118, 8, 8));
    HBRUSH titleLine = CreateSolidBrush(RGB(255, 140, 120));
    RECT titleTop{0, 2, kClientWidth, 3};
    FillRect(dc, &titleTop, titleLine);
    DeleteObject(titleLine);
    HBRUSH titleShadow = CreateSolidBrush(RGB(88, 0, 0));
    RECT titleBottom{0, kTitleBarHeight - 1, kClientWidth, kTitleBarHeight};
    FillRect(dc, &titleBottom, titleShadow);
    DeleteObject(titleShadow);
    if (g_iconLarge) {
        DrawIconEx(dc, 12, 10, g_iconLarge, 28, 28, 0, nullptr, DI_NORMAL);
    }
    SetBkMode(dc, TRANSPARENT);
    DrawTitleParts(dc, 52, 0, kTitleBarHeight, L"Net Goal ", L"11", L" LAN Play",
                   g_bannerFont, RGB(255, 245, 238), RGB(255, 210, 140));
    // Draw both caption buttons with the same visual weight.  Using font
    // glyphs made the em dash look much larger than the close cross.
    HPEN captionPen = CreatePen(PS_SOLID, 2, RGB(255, 235, 225));
    HGDIOBJ oldCaptionPen = SelectObject(dc, captionPen);
    int minCenterX = kClientWidth - 72;
    int closeCenterX = kClientWidth - 24;
    int centerY = kTitleBarHeight / 2;
    MoveToEx(dc, minCenterX - 7, centerY, nullptr);
    LineTo(dc, minCenterX + 7, centerY);
    MoveToEx(dc, closeCenterX - 6, centerY - 6, nullptr);
    LineTo(dc, closeCenterX + 6, centerY + 6);
    MoveToEx(dc, closeCenterX + 6, centerY - 6, nullptr);
    LineTo(dc, closeCenterX - 6, centerY + 6);
    SelectObject(dc, oldCaptionPen);
    DeleteObject(captionPen);

    // Stadium hero below the title bar.
    bool drewHero = false;
    if (g_heroImage && g_heroImage->GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(g_heroImage,
                           Gdiplus::RectF(0, (float)kHeroTop,
                                          (float)kClientWidth,
                                          (float)(kHeroBottom - kHeroTop)),
                           0, 0, (Gdiplus::REAL)g_heroImage->GetWidth(),
                           (Gdiplus::REAL)g_heroImage->GetHeight(), Gdiplus::UnitPixel);
        Gdiplus::LinearGradientBrush fade(
            Gdiplus::Point(0, 112), Gdiplus::Point(0, kHeroBottom),
            Gdiplus::Color(0, 7, 8, 8), Gdiplus::Color(190, 7, 8, 8));
        graphics.FillRectangle(&fade, 0, 112, kClientWidth, kHeroBottom - 112);
        drewHero = true;
    }
    if (!drewHero) {
        RECT hero{0, kHeroTop, kClientWidth, kHeroBottom};
        FillGradientRect(dc, hero, RGB(16, 34, 31), RGB(7, 8, 8));
    }

    HPEN heroLine = CreatePen(PS_SOLID, 1, RGB(155, 121, 55));
    HGDIOBJ oldPen = SelectObject(dc, heroLine);
    MoveToEx(dc, 0, kHeroBottom, nullptr);
    LineTo(dc, kClientWidth, kHeroBottom);
    SelectObject(dc, oldPen);
    DeleteObject(heroLine);

    // Gold outer border.
    HBRUSH outerBorder = CreateSolidBrush(RGB(155, 121, 55));
    FrameRect(dc, &client, outerBorder);
    DeleteObject(outerBorder);

    SetBkMode(dc, TRANSPARENT);

    // Panels.  Each region has one rounded border only.
    RECT left{8, 158, 254, 680};
    RECT center{262, 158, 782, 680};
    RECT right{790, 158, 1038, 680};
    FillRoundedRect(dc, left, RGB(16, 17, 17), RGB(105, 88, 45), 12);
    FillRoundedRect(dc, center, RGB(16, 17, 17), RGB(105, 88, 45), 12);
    FillRoundedRect(dc, right, RGB(16, 17, 17), RGB(105, 88, 45), 12);

    DrawTitlePlate(dc, 12, 160, 240, 30, L"LAN games (1)", g_font, false);
    DrawTitlePlate(dc, 790, 160, 246, 30, L"MY SETTINGS", g_font, false);
    DrawTitlePlate(dc, 282, 164, 200, 30, L"GAME ROOMS", g_font, false);

    SelectObject(dc, g_smallFont);
    SetTextColor(dc, RGB(190, 178, 150));
    TextOutW(dc, 800, 190, L"Nickname", (int)wcslen(L"Nickname"));
    TextOutW(dc, 800, 258, L"Game path (main .exe)", (int)wcslen(L"Game path (main .exe)"));
    TextOutW(dc, 800, 438, L"RUN LOG", (int)wcslen(L"RUN LOG"));
    DrawBevelFrame(dc, 800, 214, 226, 30);
    DrawBevelFrame(dc, 800, 282, 226, 30);
    DrawBevelFrame(dc, 800, 462, 226, 186);

    PaintRoomList(dc);

    // Bottom status strip.
    RECT bottom{8, 684, 1038, 728};
    FillRoundedRect(dc, bottom, RGB(16, 17, 17), RGB(105, 88, 45), 8);

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE:
            if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED)
                RepaintWholeWindow(hwnd);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) RepaintWholeWindow(hwnd);
            break;
        case WM_CREATE:
            g_hwnd = hwnd;
            CreateControls();
            DragAcceptFiles(hwnd, TRUE);
            if (!g_loginDisplayName.empty()) AppendLog(L"Signed in as: " + g_loginDisplayName);
            AppendLog(L"Client started.");
            if (StartLobbyNet(g_serverAddress, g_serverPort)) {
                SendLobbyBrowse();
            }
            SetTimer(hwnd, IDT_LOBBY_TIMER, 2000, nullptr);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_CONNECT: CreateRoomClicked(); break;
                case ID_NEW_ROOM: CreateRoomClicked(); break;
                case ID_REFRESH_ROOMS:
                    SendLobbyBrowse();
                    SetStatus(L"Refreshing the room list...");
                    break;
                case ID_JOIN_ROOM: break;
                case ID_LEAVE_ROOM:
                case ID_ROOM_LEAVE:
                    LeaveLobby(true);
                    SetStatus(L"Left the room");
                    AppendLog(L"Left the room.");
                    break;
                case ID_START_MATCH: StartGameClicked(); break;
                case ID_MODE_11: g_roomMode = 1; UpdateLobbyUi(); break;
                case ID_MODE_22: g_roomMode = 2; UpdateLobbyUi(); break;
                case ID_MODE_33: g_roomMode = 3; UpdateLobbyUi(); break;
                case ID_MODE_44: g_roomMode = 4; UpdateLobbyUi(); break;
                case ID_LAUNCH: StartGameClicked(); break;
                case ID_LOCAL_GAME: StartLocalGameClicked(); break;
                case ID_ATTACH: AttachClicked(); break;
                case ID_DISCONNECT: DisconnectClicked(); break;
                case ID_BROWSE:
                    BrowseGame();
                    if (g_inLobby) SendLobbyPing();
                    UpdateLobbyUi();
                    break;
                case ID_SAVE_CONFIG: SaveClientConfig(); break;
                case ID_CHAT_SEND: SendChat(); break;
                case ID_VERBOSE:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        g_verboseEnabled = !g_verboseEnabled;
                        InvalidateRect(g_verbose, nullptr, TRUE);
                    }
                    break;
                case ID_TAB_LOBBY: ShowTab(0); break;
                case ID_TAB_ROOM: ShowTab(1); break;
                case ID_HELP:
                    MessageBoxW(hwnd,
                                L"Net Goal 11 usage:\n\n"
                                L"1. Set your nickname on the right and choose the game executable.\n"
                                L"2. Click \"New Room\", pick 1v1-5v5 and create it; you enter the room automatically.\n"
                                L"3. Other players click \"Join\" in the room list on the home page.\n"
                                L"4. With at least 2 players, the host clicks \"Start Match\" and both games launch automatically.\n"
                                L"5. The host can kick other players from the room.",
                                L"Help", MB_OK | MB_ICONINFORMATION);
                    break;
                case ID_SETTINGS:
                    break;
                case ID_ABOUT:
                    MessageBoxW(hwnd,
                                L"Net Goal 11 LAN Play\n" NETGOAL11_VERSION L"\nNative C++ direct-connect build\n\n"
                                L"Default nickname: net11",
                                L"About", MB_OK | MB_ICONINFORMATION);
                    break;
                case ID_EXIT_APP:
                    DestroyWindow(hwnd);
                    break;
                case ID_ROOM:
                    if (HIWORD(wParam) == EN_CHANGE) {
                        UpdateRoomTree();
                    }
                    break;
                case ID_GAME:
                    if (HIWORD(wParam) == EN_CHANGE) {
                        UpdateLobbyUi();
                        if (g_inLobby) SendLobbyPing();
                    }
                    break;
                case ID_NAME:
                    if (HIWORD(wParam) == EN_CHANGE) {
                        if (g_inLobby) SendLobbyPing();
                    }
                    break;
            }
            return 0;
        case WM_DRAWITEM: {
            DrawButton((const DRAWITEMSTRUCT*)lParam);
            return TRUE;
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            if (pt.y >= 0 && pt.y < kTitleBarHeight) {
                if (pt.x >= kClientWidth - 96) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.y >= 0 && pt.y < kTitleBarHeight) {
                if (pt.x >= kClientWidth - 48) {
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (pt.x >= kClientWidth - 96) {
                    ShowWindow(hwnd, SW_MINIMIZE);
                    return 0;
                }
                break;
            }
            int index = RoomRowAt(pt);
            if (index >= 0 && !g_inLobby && !g_lobbyWaiting) {
                JoinRowClicked(index);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int row = RoomRowAt(pt);
            bool joinHover = row >= 0 && PointInRect(RoomJoinRect(row), pt);
            if (row != g_hoverRoomRow || joinHover != g_hoverJoinButton) {
                g_hoverRoomRow = row;
                g_hoverJoinButton = joinHover;
                InvalidateRoomList();
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            break;
        }
        case WM_MOUSELEAVE:
            if (g_hoverRoomRow != -1) {
                g_hoverRoomRow = -1;
                g_hoverJoinButton = false;
                InvalidateRoomList();
            }
            break;
        case WM_MOUSEWHEEL: {
            if (g_inLobby) break;
            int maxScroll = static_cast<int>(g_roomList.size()) - kRoomListVisible;
            if (maxScroll < 0) maxScroll = 0;
            if (GET_WHEEL_DELTA_WPARAM(wParam) > 0) {
                g_roomListScroll = g_roomListScroll > 0 ? g_roomListScroll - 1 : 0;
            } else {
                g_roomListScroll = g_roomListScroll < maxScroll ? g_roomListScroll + 1
                                                               : maxScroll;
            }
            InvalidateRoomList();
            break;
        }
        case WM_NOTIFY: {
            NMHDR* header = (NMHDR*)lParam;
            if (header && header->hwndFrom == g_tree &&
                header->code == NM_CUSTOMDRAW) {
                NMTVCUSTOMDRAW* draw = (NMTVCUSTOMDRAW*)lParam;
                if (draw) {
                    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        bool selected =
                            (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
                        draw->clrText = selected ? RGB(255, 227, 158)
                                                 : RGB(226, 190, 110);
                        draw->clrTextBk = selected ? RGB(40, 32, 13)
                                                   : RGB(10, 11, 11);
                        return CDRF_NEWFONT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
            if (header && header->hwndFrom == g_tree && header->code == TVN_SELCHANGEDW) {
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (wParam == IDT_LOBBY_TIMER) {
                if (g_lobbyWaiting && g_pendingLobbyType) {
                    if (g_lobbyRetry < 3) {
                        ++g_lobbyRetry;
                        SendLobbyRequest(g_pendingLobbyType);
                        AppendLog(L"The room server is not responding; retrying...");
                    } else {
                        g_lobbyWaiting = false;
                        g_pendingLobbyType = 0;
                        SetStatus(L"The room server is not responding; check your network and retry");
                        AppendLog(L"The room server is not responding; please retry.");
                        UpdateLobbyUi();
                    }
                } else if (g_inLobby) {
                    SendLobbyPing();
                } else {
                    SendLobbyBrowse();
                }
                return 0;
            }
            break;
        case WM_APP_LOBBY_EVENT: {
            auto* event = reinterpret_cast<LobbyEvent*>(lParam);
            if (event) {
                HandleLobbyEvent(*event);
                delete event;
            }
            return 0;
        }
        case WM_APP_HOOK_EVENT: {
            auto* event = reinterpret_cast<HookEvent*>(lParam);
            if (event) {
                HandleHookEvent(*event);
                delete event;
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wParam;
            wchar_t path[MAX_PATH] = {};
            if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
                SetText(g_game, path);
                ShowTab(0);
                AppendLog(L"Selected game: " + std::wstring(path));
            }
            DragFinish(drop);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            HWND control = (HWND)lParam;
            if (control == g_log || control == g_chatLog) {
                // Read-only edits also arrive here.  They must paint an opaque
                // background, otherwise scrolling leaves old glyphs behind.
                SetBkMode(dc, OPAQUE);
                SetTextColor(dc, RGB(231, 225, 210));
                SetBkColor(dc, RGB(5, 6, 6));
                return (LRESULT)g_editBrush;
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(206, 195, 173));
            return (LRESULT)g_panelBrush;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(235, 228, 211));
            SetBkColor(dc, RGB(5, 6, 6));
            return (LRESULT)g_editBrush;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(231, 225, 210));
            SetBkColor(dc, RGB(5, 6, 6));
            return (LRESULT)g_editBrush;
        }
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(225, 219, 202));
            SetBkColor(dc, RGB(15, 17, 17));
            return (LRESULT)g_panelBrush;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintWindow(hwnd);
            return 0;
        case WM_CLOSE:
            // Direct-mode hook owns the relay.  Closing the GUI must not cut
            // the game's connection; use the “断开” button for that.
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, IDT_LOBBY_TIMER);
            if (g_inLobby) SendLobbyPacket(fr::LobbyLeave, nullptr, 0);
            g_inLobby = false;
            CloseRoomWindow();
            StopLobbyNet();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_instance = instance;
    SetProcessDPIAware();

    // Open source build: no license file.  The login window asks for the
    // server address plus a free-form nickname; the shared password is fixed.
    LoginResult login;
    if (!ShowLoginWindow(instance, L"", L"net11", L"goal11", login)) {
        return 0;
    }
    g_loginDisplayName = login.displayName.empty() ? login.username : login.displayName;
    if (g_loginDisplayName.empty()) g_loginDisplayName = L"Player";
    g_serverAddress = login.server;
    g_serverPort = login.port;

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) ==
        Gdiplus::Ok) {
        std::wstring heroPath = ExecutableDirectory() + L"\\main_banner.jpg";
        g_heroImage = Gdiplus::Image::FromFile(heroPath.c_str());
        if (!g_heroImage || g_heroImage->GetLastStatus() != Gdiplus::Ok) {
            delete g_heroImage;
            g_heroImage = nullptr;
            heroPath = ExecutableDirectory() + L"\\login_bg.jpg";
            g_heroImage = Gdiplus::Image::FromFile(heroPath.c_str());
            if (g_heroImage && g_heroImage->GetLastStatus() != Gdiplus::Ok) {
                delete g_heroImage;
                g_heroImage = nullptr;
            }
        }
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed", L"Net Goal 11", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_relayReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    std::wstring iconPath = ExecutableDirectory() + L"\\app.ico";
    g_iconLarge = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 32, 32,
                                    LR_LOADFROMFILE);
    g_iconSmall = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 16, 16,
                                    LR_LOADFROMFILE);

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    cls.lpszClassName = L"NetGoal11Client";
    cls.hIcon = g_iconLarge ? g_iconLarge : LoadIconW(nullptr, IDI_APPLICATION);
    cls.hIconSm = g_iconSmall ? g_iconSmall : cls.hIcon;
    if (!RegisterClassExW(&cls)) {
        MessageBoxW(nullptr, L"Failed to register the window class", L"Net Goal 11", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    std::wstring title = L"Net Goal 11 LAN Play " NETGOAL11_VERSION;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_COMPOSITED,
                                cls.lpszClassName, title.c_str(),
                                WS_POPUP | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                kClientWidth, kClientHeight,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create the window", L"Net Goal 11", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            GetFocus() == g_chatInput) {
            SendChat();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopIpc();
    if (g_relayReadyEvent) CloseHandle(g_relayReadyEvent);
    if (g_heroImage) {
        delete g_heroImage;
        g_heroImage = nullptr;
    }
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    if (g_iconLarge) DestroyIcon(g_iconLarge);
    if (g_iconSmall) DestroyIcon(g_iconSmall);
    g_iconLarge = g_iconSmall = nullptr;
    if (g_font) DeleteObject(g_font);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_bannerFont) DeleteObject(g_bannerFont);
    if (g_smallFont) DeleteObject(g_smallFont);
    if (g_windowBrush) DeleteObject(g_windowBrush);
    if (g_panelBrush) DeleteObject(g_panelBrush);
    if (g_editBrush) DeleteObject(g_editBrush);
    if (g_whiteBrush) DeleteObject(g_whiteBrush);
    WSACleanup();
    return (int)msg.wParam;
}
