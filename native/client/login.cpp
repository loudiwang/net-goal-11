// Native Win32 login window for the Net Goal 11 client.
//
// This build is open source: there is no license file and no registration
// server.  The player enters the LAN server IP and port, a nickname and the
// shared access password; everything is validated locally in this process.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <wincrypt.h>
#include <propidl.h>
#include <gdiplus.h>

#include "login.h"
#include "../common/version.h"

#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr int kWidth = 1120;
constexpr int kHeight = 700;
constexpr int kPanelX = 660;
constexpr int kFieldX = 710;
constexpr int kFieldW = 380;
constexpr int kFieldH = 56;
constexpr int kInputX = 810;
constexpr int kInputW = 260;
constexpr int kServerY = 172;
constexpr int kPortY = 244;
constexpr int kAccountY = 316;
constexpr int kPasswordY = 388;
constexpr int kLoginY = 460;
constexpr int kStatusY = 544;
constexpr int kTopBar = 44;
constexpr int kChromeSize = 32;
constexpr int kChromeTop = 6;
constexpr int kChromeRight = 8;
constexpr int kChromeGap = 4;

RECT CloseButtonRect() {
    RECT rc{kWidth - kChromeRight - kChromeSize, kChromeTop,
            kWidth - kChromeRight, kChromeTop + kChromeSize};
    return rc;
}

RECT MinimizeButtonRect() {
    RECT rc = CloseButtonRect();
    rc.left -= kChromeGap + kChromeSize;
    rc.right = rc.left + kChromeSize;
    return rc;
}

void DrawChromeGlyph(HDC dc, const RECT& rc, bool isClose) {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(226, 190, 110));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (isClose) {
        MoveToEx(dc, cx - 6, cy - 6, nullptr);
        LineTo(dc, cx + 7, cy + 7);
        MoveToEx(dc, cx + 6, cy - 6, nullptr);
        LineTo(dc, cx - 7, cy + 7);
    } else {
        MoveToEx(dc, cx - 7, cy, nullptr);
        LineTo(dc, cx + 8, cy);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// A faint dark gauze keeps the gold labels readable while the artwork still
// shows through the account/password fields.
constexpr int kFieldVeilAlpha = 92;
constexpr COLORREF kFieldVeilColor = RGB(6, 10, 9);

constexpr COLORREF kPanelColor = RGB(3, 20, 18);
constexpr COLORREF kLineColor = RGB(176, 139, 70);
constexpr COLORREF kLabelColor = RGB(226, 190, 110);
constexpr COLORREF kTextColor = RGB(235, 249, 244);
constexpr COLORREF kErrorColor = RGB(255, 112, 102);

constexpr wchar_t kDefaultUser[] = L"net11";
constexpr wchar_t kDefaultPassword[] = L"goal11";
constexpr wchar_t kDefaultServer[] = L"127.0.0.1";
constexpr int kDefaultPort = 45888;

enum ControlId {
    ID_SERVER = 2000,
    ID_PORT,
    ID_ACCOUNT,
    ID_PASSWORD,
    ID_REMEMBER,
    ID_LOGIN,
};

HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
HWND g_server = nullptr;
HWND g_port = nullptr;
HWND g_account = nullptr;
HWND g_password = nullptr;
HWND g_rememberCheck = nullptr;
HWND g_login = nullptr;
HWND g_status = nullptr;

HFONT g_font = nullptr;
HFONT g_logoFont = nullptr;
HFONT g_logoSubFont = nullptr;
HFONT g_loginFont = nullptr;
HFONT g_smallFont = nullptr;

HBRUSH g_panelBrush = nullptr;
HBRUSH g_editBrush = nullptr;

Gdiplus::Bitmap* g_background = nullptr;
ULONG_PTR g_gdiplusToken = 0;

HICON g_iconLarge = nullptr;
HICON g_iconSmall = nullptr;

WNDPROC g_serverProc = nullptr;
WNDPROC g_portProc = nullptr;
WNDPROC g_accountProc = nullptr;
WNDPROC g_passwordProc = nullptr;

std::wstring g_savedPassword;
std::wstring g_savedServer;
std::wstring g_savedUser;
int g_savedPort = 0;
std::wstring g_expectedUser;
std::wstring g_expectedPassword;
bool g_remember = false;
bool g_statusError = false;
bool g_done = false;
LoginResult* g_result = nullptr;

const wchar_t* kRegistryPath = L"Software\\Net Goal 11\\Login";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::wstring Trim(const std::wstring& text) {
    size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring GetText(HWND control) {
    if (!control) return {};
    int length = GetWindowTextLengthW(control);
    std::wstring text((size_t)length + 1, L'\0');
    if (length > 0) GetWindowTextW(control, text.data(), length + 1);
    text.resize((size_t)length);
    return text;
}

void SetText(HWND control, const std::wstring& text) {
    if (control) SetWindowTextW(control, text.c_str());
}

bool IsChecked(HWND control) {
    return control && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L".";
    std::wstring result(path, length);
    size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return result.substr(0, slash);
}

void SetStatus(const std::wstring& text, bool error) {
    if (!g_status) return;
    g_statusError = error;
    SetWindowTextW(g_status, text.c_str());
    ShowWindow(g_status, text.empty() ? SW_HIDE : SW_SHOW);
    InvalidateRect(g_status, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Remembered password (DPAPI-protected, per Windows user)
// ---------------------------------------------------------------------------
bool ReadRegString(HKEY key, const wchar_t* name, std::wstring& value) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_SZ || size < sizeof(wchar_t)) {
        return false;
    }
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)text.data(), &size) != ERROR_SUCCESS) {
        return false;
    }
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    value = text;
    return true;
}

bool ReadRegDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD type = 0;
    DWORD size = sizeof(value);
    return RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS &&
           type == REG_DWORD;
}

void LoadSavedLogin() {
    g_savedPassword.clear();
    g_savedServer.clear();
    g_savedUser.clear();
    g_savedPort = 0;
    // Default to remembering the fixed login; the checkbox is checked when the
    // login window opens.
    g_remember = true;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }

    if (g_remember) {
        if (ReadRegString(key, L"username", g_savedUser)) {
            g_savedUser = Trim(g_savedUser);
        }
        if (ReadRegString(key, L"server", g_savedServer)) {
            g_savedServer = Trim(g_savedServer);
        }
        DWORD port = 0;
        if (ReadRegDword(key, L"port", port) && port > 0 && port <= 65535) {
            g_savedPort = (int)port;
        }
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key, L"password", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
            type == REG_BINARY && size > 0) {
            std::vector<BYTE> blob(size);
            if (RegQueryValueExW(key, L"password", nullptr, &type, blob.data(), &size) ==
                ERROR_SUCCESS) {
                DATA_BLOB input{};
                input.cbData = size;
                input.pbData = blob.data();
                DATA_BLOB output{};
                if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                       CRYPTPROTECT_UI_FORBIDDEN, &output)) {
                    size_t chars = output.cbData / sizeof(wchar_t);
                    if (chars > 0) {
                        g_savedPassword.assign((wchar_t*)output.pbData, chars);
                    }
                    LocalFree(output.pbData);
                }
            }
        }
    }

    RegCloseKey(key);
}

void DeleteSavedPassword() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"password");
        DWORD zero = 0;
        RegSetValueExW(key, L"remember", 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
        RegCloseKey(key);
    }
}

void SaveLoginSettings() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    std::wstring account = Trim(GetText(g_account));
    if (account.empty()) account = g_expectedUser.empty() ? kDefaultUser : g_expectedUser;
    RegSetValueExW(key, L"username", 0, REG_SZ, (const BYTE*)account.c_str(),
                   (DWORD)((account.size() + 1) * sizeof(wchar_t)));

    std::wstring server = Trim(GetText(g_server));
    if (!server.empty()) {
        RegSetValueExW(key, L"server", 0, REG_SZ, (const BYTE*)server.c_str(),
                       (DWORD)((server.size() + 1) * sizeof(wchar_t)));
    }
    int port = _wtoi(Trim(GetText(g_port)).c_str());
    DWORD portValue = (DWORD)(port > 0 ? port : kDefaultPort);
    RegSetValueExW(key, L"port", 0, REG_DWORD, (const BYTE*)&portValue, sizeof(portValue));

    DWORD remember = g_remember ? 1 : 0;
    RegSetValueExW(key, L"remember", 0, REG_DWORD, (const BYTE*)&remember, sizeof(remember));

    if (g_remember) {
        std::wstring password = GetText(g_password);
        if (!password.empty()) {
            DATA_BLOB input{};
            input.cbData = (DWORD)(password.size() * sizeof(wchar_t));
            input.pbData = (BYTE*)password.data();
            DATA_BLOB output{};
            if (CryptProtectData(&input, L"Net Goal 11 login", nullptr, nullptr, nullptr,
                                 CRYPTPROTECT_UI_FORBIDDEN, &output)) {
                RegSetValueExW(key, L"password", 0, REG_BINARY, output.pbData, output.cbData);
                LocalFree(output.pbData);
            }
        }
    } else {
        RegDeleteValueW(key, L"password");
    }

    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Local login (open source build, no registration)
// ---------------------------------------------------------------------------
void DoLogin() {
    std::wstring server = Trim(GetText(g_server));
    std::wstring portText = Trim(GetText(g_port));
    std::wstring account = Trim(GetText(g_account));
    std::wstring password = GetText(g_password);

    if (server.empty()) {
        SetFocus(g_server);
        SetStatus(L"请输入服务器 IP", true);
        return;
    }
    int port = _wtoi(portText.c_str());
    if (portText.empty() || port < 1 || port > 65535) {
        SetFocus(g_port);
        SetStatus(L"请输入 1-65535 之间的端口", true);
        return;
    }
    if (account.empty()) {
        SetFocus(g_account);
        SetStatus(L"请输入昵称", true);
        return;
    }
    if (password.empty()) {
        SetFocus(g_password);
        SetStatus(L"请输入密码", true);
        return;
    }

    // The nickname is free-form; only the shared access password is fixed.
    std::wstring expectedPassword = g_expectedPassword.empty() ? kDefaultPassword
                                                               : g_expectedPassword;
    if (password != expectedPassword) {
        SetStatus(L"密码错误", true);
        SetFocus(g_password);
        SendMessageW(g_password, EM_SETSEL, 0, -1);
        return;
    }

    g_result->username = account;
    g_result->displayName = account;
    g_result->token = L"local";
    g_result->apiBase.clear();
    g_result->server = server;
    g_result->port = port;

    if (g_rememberCheck) g_remember = IsChecked(g_rememberCheck);
    SaveLoginSettings();
    g_done = true;
    DestroyWindow(g_hwnd);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void FillGradient(HDC dc, const RECT& rect, COLORREF top, COLORREF bottom) {
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

void DrawInputFrame(HDC dc, int x, int y, int w, int h) {
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

void FillRoundedRectAlpha(HDC dc, const RECT& rc, int alpha, COLORREF fill,
                          int radius) {
    int d = radius * 2;
    Gdiplus::GraphicsPath path;
    path.AddArc((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top, (Gdiplus::REAL)d,
                (Gdiplus::REAL)d, 180.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)(rc.right - d), (Gdiplus::REAL)rc.top,
                (Gdiplus::REAL)d, (Gdiplus::REAL)d, 270.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)(rc.right - d), (Gdiplus::REAL)(rc.bottom - d),
                (Gdiplus::REAL)d, (Gdiplus::REAL)d, 0.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)rc.left, (Gdiplus::REAL)(rc.bottom - d),
                (Gdiplus::REAL)d, (Gdiplus::REAL)d, 90.0f, 90.0f);
    path.CloseFigure();

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(Gdiplus::Color(
        (BYTE)alpha, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
    graphics.FillPath(&brush, &path);
}

void DrawLoginField(HDC dc, int x, int y, int w, int h, const wchar_t* text) {
    RECT rc{x, y, x + w, y + h};
    // Faint gauze only; the artwork is still clearly visible through it.
    FillRoundedRectAlpha(dc, rc, kFieldVeilAlpha, kFieldVeilColor, 16);
    HPEN frame = CreatePen(PS_SOLID, 1, RGB(171, 158, 119));
    HGDIOBJ oldFrame = SelectObject(dc, frame);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 32, 32);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldFrame);
    DeleteObject(frame);
    HPEN light = CreatePen(PS_SOLID, 1, RGB(211, 199, 166));
    HPEN dark = CreatePen(PS_SOLID, 1, RGB(62, 54, 36));
    HGDIOBJ old = SelectObject(dc, light);
    MoveToEx(dc, x + 16, y + 1, nullptr);
    LineTo(dc, x + w - 16, y + 1);
    SelectObject(dc, dark);
    MoveToEx(dc, x + 16, y + h - 2, nullptr);
    LineTo(dc, x + w - 16, y + h - 2);
    SelectObject(dc, old);
    DeleteObject(dark);
    DeleteObject(light);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);
    RECT textRect{x + 24, y, x + 116, y + h};
    RECT shadowRect = textRect;
    OffsetRect(&shadowRect, 1, 1);
    SetTextColor(dc, RGB(10, 12, 10));
    DrawTextW(dc, text, -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, RGB(226, 190, 110));
    DrawTextW(dc, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// The edits are borderless and must look fully transparent, so paint the
// artwork slice behind them before the control draws its gold text.
void PaintEditBackdrop(HWND edit) {
    RECT rc{};
    GetClientRect(edit, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    POINT origin{0, 0};
    MapWindowPoints(edit, GetParent(edit), &origin, 1);

    HDC dc = GetDC(edit);
    if (g_background && g_background->GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(g_background, (Gdiplus::REAL)-origin.x,
                           (Gdiplus::REAL)-origin.y, (Gdiplus::REAL)kWidth,
                           (Gdiplus::REAL)kHeight);
        Gdiplus::SolidBrush veil(Gdiplus::Color(150, 3, 20, 18));
        graphics.FillRectangle(&veil, 0, 0, rc.right, rc.bottom);
        Gdiplus::SolidBrush gauze(Gdiplus::Color(
            kFieldVeilAlpha, GetRValue(kFieldVeilColor),
            GetGValue(kFieldVeilColor), GetBValue(kFieldVeilColor)));
        graphics.FillRectangle(&gauze, 0, 0, rc.right, rc.bottom);
    } else {
        HBRUSH brush = CreateSolidBrush(RGB(10, 11, 11));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
    }
    ReleaseDC(edit, dc);
}

LRESULT CALLBACK LoginEditProc(HWND hwnd, UINT message, WPARAM wParam,
                               LPARAM lParam) {
    WNDPROC previous = nullptr;
    if (hwnd == g_server) previous = g_serverProc;
    else if (hwnd == g_port) previous = g_portProc;
    else if (hwnd == g_password) previous = g_passwordProc;
    else previous = g_accountProc;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) PaintEditBackdrop(hwnd);
    return previous ? CallWindowProcW(previous, hwnd, message, wParam, lParam)
                    : DefWindowProcW(hwnd, message, wParam, lParam);
}

void DrawLoginLabel(HDC dc, int x, int y, int w, int h, const wchar_t* text) {
    RECT plate{x, y, x + w, y + h};
    FillRoundedRect(dc, plate, RGB(18, 19, 18), RGB(150, 116, 51), 6);
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
    SetTextColor(dc, kLabelColor);
    SelectObject(dc, g_smallFont);
    DrawTextW(dc, text, -1, &plate, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PaintLogin(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    bool drewBackground = false;
    if (g_background && g_background->GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(g_background, 0, 0, kWidth, kHeight);
        drewBackground = true;
    }
    if (!drewBackground) {
        FillGradient(dc, client, RGB(18, 58, 49), RGB(2, 13, 12));
    }

    // A translucent sheet lets the stadium image remain faintly visible
    // behind the right-hand login column.
    {
        Gdiplus::Graphics graphics(dc);
        Gdiplus::SolidBrush overlay(Gdiplus::Color(150, 3, 20, 18));
        graphics.FillRectangle(&overlay, kPanelX, 0, kWidth - kPanelX, kHeight);
    }

    HPEN divider = CreatePen(PS_SOLID, 1, RGB(203, 165, 80));
    HGDIOBJ oldPen = SelectObject(dc, divider);
    MoveToEx(dc, kPanelX, 0, nullptr);
    LineTo(dc, kPanelX, kHeight);
    SelectObject(dc, oldPen);
    DeleteObject(divider);

    DrawLoginField(dc, kFieldX, kServerY, kFieldW, kFieldH, L"服务器 IP");
    DrawLoginField(dc, kFieldX, kPortY, kFieldW, kFieldH, L"端口");
    DrawLoginField(dc, kFieldX, kAccountY, kFieldW, kFieldH, L"昵称");
    DrawLoginField(dc, kFieldX, kPasswordY, kFieldW, kFieldH, L"密码");

    SelectObject(dc, g_logoFont);
    auto drawLogoPart = [&](const wchar_t* text, int x, int y, COLORREF color) {
        int length = (int)wcslen(text);
        SetTextColor(dc, RGB(0, 0, 0));
        TextOutW(dc, x + 2, y + 3, text, length);
        SetTextColor(dc, color);
        TextOutW(dc, x, y, text, length);
    };

    int logoX = kFieldX;
    const int logoY = 45;
    const wchar_t* logoText = L"Net Goal";
    const int logoLen = (int)wcslen(logoText);
    drawLogoPart(logoText, logoX, logoY, RGB(255, 255, 255));
    SIZE partSize{};
    GetTextExtentPoint32W(dc, logoText, logoLen, &partSize);
    int logoEnd = kFieldX + partSize.cx + 8;
    SIZE numberSize{};
    GetTextExtentPoint32W(dc, L"11", 2, &numberSize);
    logoEnd += numberSize.cx;
    drawLogoPart(L"11", kFieldX + partSize.cx + 8, logoY, RGB(238, 57, 50));

    // The tagline spans the exact width of the logo above it. GDI ignores
    // negative inter-character spacing, so drive the glyph advances directly.
    // The "11" is drawn in the same red as the logo numeral.
    const wchar_t* subtitle = L"登录Net Goal 11对战平台";
    const int subtitleLen = (int)wcslen(subtitle);
    const std::wstring subtitleText(subtitle);
    size_t foundNumber = subtitleText.find(L"11");
    const int numberStart = foundNumber == std::wstring::npos ? 5 : (int)foundNumber;
    const int numberLen = 2;     // "11"
    SelectObject(dc, g_logoSubFont);
    int advances[64] = {};
    int natural = 0;
    if (subtitleLen > 0 && subtitleLen < 64) {
        int previous = 0;
        for (int i = 0; i < subtitleLen; ++i) {
            SIZE prefix{};
            GetTextExtentPoint32W(dc, subtitle, i + 1, &prefix);
            advances[i] = prefix.cx - previous;
            previous = prefix.cx;
        }
        natural = previous;
    }
    const int target = logoEnd - kFieldX;
    if (natural > 0 && subtitleLen > 0 && subtitleLen < 64) {
        double scale = (double)target / (double)natural;
        int used = 0;
        for (int i = 0; i < subtitleLen; ++i) {
            int step = (int)(advances[i] * scale + 0.5);
            if (step < 1) step = 1;
            if (i == subtitleLen - 1) step = target - used;
            advances[i] = step;
            used += step;
        }
    }
    int numberX = kFieldX;
    for (int i = 0; i < numberStart; ++i) numberX += advances[i];
    SetTextColor(dc, RGB(226, 190, 110));
    ExtTextOutW(dc, kFieldX, 102, 0, nullptr, subtitle, numberStart, advances);
    SetTextColor(dc, RGB(238, 57, 50));
    ExtTextOutW(dc, numberX, 102, 0, nullptr, subtitle + numberStart, numberLen,
                advances + numberStart);
    int suffixX = numberX;
    for (int i = numberStart; i < numberStart + numberLen; ++i) suffixX += advances[i];
    SetTextColor(dc, RGB(226, 190, 110));
    ExtTextOutW(dc, suffixX, 102, 0, nullptr, subtitle + numberStart + numberLen,
                subtitleLen - numberStart - numberLen, advances + numberStart + numberLen);

    // Minimal window chrome: identical boxes and stroke weight for both glyphs.
    RECT minRect = MinimizeButtonRect();
    RECT closeRect = CloseButtonRect();
    DrawChromeGlyph(dc, minRect, false);
    DrawChromeGlyph(dc, closeRect, true);

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
void ApplyFont(HWND control, HFONT font) {
    if (control) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

HWND MakeLabel(const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   x, y, w, h, g_hwnd, nullptr, g_instance, nullptr);
    ApplyFont(control, g_smallFont);
    return control;
}

HWND MakeDarkEdit(int id, int x, int y, int w, int h, DWORD extra = 0) {
    HWND control = CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                                   x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, g_instance, nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND MakeButton(int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, g_instance, nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND MakeCheck(int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       BS_AUTOCHECKBOX | BS_OWNERDRAW,
                                   x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, g_instance, nullptr);
    ApplyFont(control, g_smallFont);
    return control;
}

void CreateControls() {
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_logoFont = CreateFontW(-47, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Arial Black");
    g_logoSubFont = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_loginFont = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    g_panelBrush = CreateSolidBrush(kPanelColor);
    g_editBrush = CreateSolidBrush(RGB(25, 27, 26));

    // Borderless fields; the underline is drawn by PaintLogin, not by the edit.
    g_server = MakeDarkEdit(ID_SERVER, kInputX, kServerY + 13,
                            kInputW, 30);
    g_port = MakeDarkEdit(ID_PORT, kInputX, kPortY + 13,
                          kInputW, 30, ES_NUMBER);
    g_account = MakeDarkEdit(ID_ACCOUNT, kInputX, kAccountY + 13,
                             kInputW, 30);
    g_password = MakeDarkEdit(ID_PASSWORD, kInputX, kPasswordY + 13,
                              kInputW, 30, ES_PASSWORD);
    SendMessageW(g_server, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(6, 6));
    SendMessageW(g_port, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(6, 6));
    SendMessageW(g_account, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(6, 6));
    SendMessageW(g_password, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(6, 6));
    // Subclass the edits so they paint the artwork behind their text and
    // look like part of the transparent field.
    g_serverProc = (WNDPROC)SetWindowLongPtrW(g_server, GWLP_WNDPROC,
                                              (LONG_PTR)LoginEditProc);
    g_portProc = (WNDPROC)SetWindowLongPtrW(g_port, GWLP_WNDPROC,
                                            (LONG_PTR)LoginEditProc);
    g_accountProc = (WNDPROC)SetWindowLongPtrW(g_account, GWLP_WNDPROC,
                                               (LONG_PTR)LoginEditProc);
    g_passwordProc = (WNDPROC)SetWindowLongPtrW(g_password, GWLP_WNDPROC,
                                                (LONG_PTR)LoginEditProc);

    g_login = MakeButton(ID_LOGIN, L"登 录", kFieldX, kLoginY, kFieldW, 66);

    g_status = MakeLabel(L"", kFieldX, kStatusY, kFieldW, 22);

    LoadSavedLogin();
    SetText(g_server, g_savedServer.empty() ? kDefaultServer : g_savedServer);
    SetText(g_port, std::to_wstring(g_savedPort > 0 ? g_savedPort : kDefaultPort));
    if (!g_savedUser.empty()) {
        SetText(g_account, g_savedUser);
    } else {
        SetText(g_account, g_expectedUser.empty() ? kDefaultUser : g_expectedUser);
    }
    SetText(g_password, g_expectedPassword.empty() ? kDefaultPassword : g_expectedPassword);
    SetStatus(L"", false);
}

// ---------------------------------------------------------------------------
// Owner-drawn controls
// ---------------------------------------------------------------------------
void DrawLoginButton(HDC dc, const RECT& rc, bool pressed, HWND hwndItem) {
    int radius = 28;
    int d = radius * 2;
    Gdiplus::GraphicsPath path;
    path.AddArc((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top, (Gdiplus::REAL)d,
                (Gdiplus::REAL)d, 180.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)(rc.right - d), (Gdiplus::REAL)rc.top, (Gdiplus::REAL)d,
                (Gdiplus::REAL)d, 270.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)(rc.right - d), (Gdiplus::REAL)(rc.bottom - d),
                (Gdiplus::REAL)d, (Gdiplus::REAL)d, 0.0f, 90.0f);
    path.AddArc((Gdiplus::REAL)rc.left, (Gdiplus::REAL)(rc.bottom - d), (Gdiplus::REAL)d,
                (Gdiplus::REAL)d, 90.0f, 90.0f);
    path.CloseFigure();

    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // Owner-drawn buttons have no backdrop of their own, so paint the
        // window artwork underneath before the rounded gold shape. Without
        // this the four corners would show through as white blocks.
        POINT origin{0, 0};
        HWND parent = GetParent(hwndItem);
        if (parent) MapWindowPoints(hwndItem, parent, &origin, 1);
        if (g_background && g_background->GetLastStatus() == Gdiplus::Ok) {
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            graphics.DrawImage(g_background, (Gdiplus::REAL)-origin.x,
                               (Gdiplus::REAL)-origin.y,
                               (Gdiplus::REAL)kWidth, (Gdiplus::REAL)kHeight);
        } else {
            Gdiplus::SolidBrush base(Gdiplus::Color(255, 8, 34, 29));
            graphics.FillRectangle(&base, 0, 0, rc.right, rc.bottom);
        }
        Gdiplus::SolidBrush veil(Gdiplus::Color(150, 3, 20, 18));
        graphics.FillRectangle(&veil, 0, 0, rc.right, rc.bottom);

        Gdiplus::Color top = pressed ? Gdiplus::Color(255, 170, 30, 26)
                                     : Gdiplus::Color(255, 232, 72, 58);
        Gdiplus::Color bottom = pressed ? Gdiplus::Color(255, 95, 5, 5)
                                        : Gdiplus::Color(255, 140, 10, 10);
        Gdiplus::LinearGradientBrush brush(
            Gdiplus::Point(rc.left, rc.top), Gdiplus::Point(rc.left, rc.bottom),
            top, bottom);
        graphics.FillPath(&brush, &path);
        Gdiplus::Pen border(Gdiplus::Color(255, 255, 130, 110), 2.0f);
        graphics.DrawPath(&border, &path);
        Gdiplus::Pen highlight(Gdiplus::Color(210, 255, 160, 138), 1.0f);
        graphics.DrawLine(&highlight, (Gdiplus::REAL)(rc.left + 20),
                          (Gdiplus::REAL)(rc.top + 4),
                          (Gdiplus::REAL)(rc.right - 20),
                          (Gdiplus::REAL)(rc.top + 4));
        Gdiplus::Pen shadow(Gdiplus::Color(220, 92, 0, 0), 1.0f);
        graphics.DrawLine(&shadow, (Gdiplus::REAL)(rc.left + 20),
                          (Gdiplus::REAL)(rc.bottom - 4),
                          (Gdiplus::REAL)(rc.right - 20),
                          (Gdiplus::REAL)(rc.bottom - 4));
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 246, 240));
    SelectObject(dc, g_loginFont);
    RECT textRect = rc;
    if (pressed) OffsetRect(&textRect, 0, 1);
    DrawTextW(dc, L"登 录", -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawButton(const DRAWITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_BUTTON) return;
    HDC dc = item->hDC;
    RECT rc = item->rcItem;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;

    if (item->CtlID == ID_LOGIN) {
        DrawLoginButton(dc, rc, pressed, item->hwndItem);
        return;
    }

    if (item->CtlID == ID_REMEMBER) {
        // Same color as the panel: the save row has no green plate behind it.
        FillRect(dc, &rc, g_panelBrush);

        bool checked = g_remember;
        int boxSize = 18;
        RECT box{rc.left, rc.top + (rc.bottom - rc.top - boxSize) / 2,
                 rc.left + boxSize, rc.top + (rc.bottom - rc.top - boxSize) / 2 + boxSize};
        FillRoundedRect(dc, box, checked ? RGB(91, 224, 194) : kPanelColor,
                        checked ? RGB(128, 241, 213) : RGB(85, 130, 118), 5);
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(5, 42, 36));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, box.left + 4, box.top + 9, nullptr);
            LineTo(dc, box.left + 8, box.top + 13);
            LineTo(dc, box.left + 15, box.top + 5);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(225, 241, 235));
        SelectObject(dc, g_smallFont);
        RECT textRect{box.right + 8, rc.top, rc.right, rc.bottom};
        DrawTextW(dc, GetText(item->hwndItem).c_str(), -1, &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    COLORREF background = pressed ? RGB(140, 18, 16) : RGB(205, 48, 40);
    COLORREF border = pressed ? RGB(255, 150, 128) : RGB(255, 128, 108);
    COLORREF text = RGB(255, 246, 240);

    FillRoundedRect(dc, rc, background, border, 8);
    if (item->CtlID == ID_LOGIN) {
        HPEN highlight = CreatePen(PS_SOLID, 2, RGB(255, 150, 128));
        HGDIOBJ oldPen = SelectObject(dc, highlight);
        MoveToEx(dc, rc.left + 18, rc.top + 4, nullptr);
        LineTo(dc, rc.right - 18, rc.top + 4);
        SelectObject(dc, oldPen);
        DeleteObject(highlight);

        HPEN shadow = CreatePen(PS_SOLID, 2, RGB(92, 0, 0));
        oldPen = SelectObject(dc, shadow);
        MoveToEx(dc, rc.left + 18, rc.bottom - 5, nullptr);
        LineTo(dc, rc.right - 18, rc.bottom - 5);
        SelectObject(dc, oldPen);
        DeleteObject(shadow);
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    SelectObject(dc, item->CtlID == ID_LOGIN ? g_loginFont : g_font);
    RECT textRect = rc;
    if (pressed) OffsetRect(&textRect, 0, 1);
    DrawTextW(dc, GetText(item->hwndItem).c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK LoginWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_hwnd = hwnd;
            CreateControls();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_LOGIN:
                    if (HIWORD(wParam) == BN_CLICKED) DoLogin();
                    return 0;
                case ID_REMEMBER:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        g_remember = IsChecked(g_rememberCheck);
                        if (!g_remember) DeleteSavedPassword();
                        InvalidateRect(g_rememberCheck, nullptr, TRUE);
                    }
                    return 0;
            }
            return 0;
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = (const DRAWITEMSTRUCT*)lParam;
            DrawButton(item);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, g_statusError ? kErrorColor : RGB(226, 190, 110));
            // The only static is the status line; keep its small background
            // opaque so changing error text does not leave artifacts.
            return (LRESULT)g_panelBrush;
        }
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kTextColor);
            return (LRESULT)GetStockObject(HOLLOW_BRUSH);
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(226, 190, 110));
            // Hollow brush keeps the painted artwork visible behind the text.
            return (LRESULT)GetStockObject(HOLLOW_BRUSH);
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            if (pt.y >= 0 && pt.y < kTopBar) {
                RECT minRect = MinimizeButtonRect();
                RECT closeRect = CloseButtonRect();
                if (PtInRect(&minRect, pt) || PtInRect(&closeRect, pt)) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.y >= 0 && pt.y < kTopBar) {
                RECT minRect = MinimizeButtonRect();
                RECT closeRect = CloseButtonRect();
                if (PtInRect(&closeRect, pt)) {
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (PtInRect(&minRect, pt)) {
                    ShowWindow(hwnd, SW_MINIMIZE);
                    return 0;
                }
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintLogin(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

bool ShowLoginWindow(HINSTANCE instance, const std::wstring& defaultApiBase,
                     const std::wstring& username, const std::wstring& password,
                     LoginResult& result) {
    (void)defaultApiBase;

    g_instance = instance;
    g_result = &result;
    g_expectedUser = username;
    g_expectedPassword = password;
    g_done = false;

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) ==
        Gdiplus::Ok) {
        std::wstring backgroundPath = ExeDirectory() + L"\\login_bg.jpg";
        g_background = Gdiplus::Bitmap::FromFile(backgroundPath.c_str());
        if (g_background && g_background->GetLastStatus() != Gdiplus::Ok) {
            delete g_background;
            g_background = nullptr;
        }
    }

    std::wstring iconPath = ExeDirectory() + L"\\app.ico";
    g_iconLarge = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 32, 32,
                                   LR_LOADFROMFILE);
    g_iconSmall = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 16, 16,
                                   LR_LOADFROMFILE);

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = LoginWndProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    cls.lpszClassName = L"NetGoal11LoginWindow";
    cls.hIcon = g_iconLarge ? g_iconLarge : LoadIconW(nullptr, IDI_APPLICATION);
    cls.hIconSm = g_iconSmall ? g_iconSmall : cls.hIcon;
    RegisterClassExW(&cls);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + (work.right - work.left - kWidth) / 2;
    int y = work.top + (work.bottom - work.top - kHeight) / 2;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, cls.lpszClassName,
        L"Net Goal 11 对战系统 " NETGOAL11_VERSION L" - 登录", WS_POPUP | WS_CLIPCHILDREN,
        x, y, kWidth, kHeight, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        if (g_background) {
            delete g_background;
            g_background = nullptr;
        }
        if (g_gdiplusToken) {
            Gdiplus::GdiplusShutdown(g_gdiplusToken);
            g_gdiplusToken = 0;
        }
        if (g_iconLarge) {
            DestroyIcon(g_iconLarge);
            g_iconLarge = nullptr;
        }
        if (g_iconSmall) {
            DestroyIcon(g_iconSmall);
            g_iconSmall = nullptr;
        }
        g_result = nullptr;
        return false;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    if (Trim(GetText(g_server)).empty()) {
        SetFocus(g_server);
    } else {
        SetFocus(g_account);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            continue;
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_background) {
        delete g_background;
        g_background = nullptr;
    }
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    if (g_font) DeleteObject(g_font);
    if (g_logoFont) DeleteObject(g_logoFont);
    if (g_logoSubFont) DeleteObject(g_logoSubFont);
    if (g_loginFont) DeleteObject(g_loginFont);
    if (g_smallFont) DeleteObject(g_smallFont);
    if (g_panelBrush) DeleteObject(g_panelBrush);
    if (g_editBrush) DeleteObject(g_editBrush);
    if (g_iconLarge) {
        DestroyIcon(g_iconLarge);
        g_iconLarge = nullptr;
    }
    if (g_iconSmall) {
        DestroyIcon(g_iconSmall);
        g_iconSmall = nullptr;
    }
    g_font = g_logoFont = g_logoSubFont = g_loginFont = g_smallFont = nullptr;
    g_panelBrush = g_editBrush = nullptr;
    g_result = nullptr;
    return g_done;
}
