#pragma once

#include <windows.h>
#include <string>

struct LoginResult {
    std::wstring username;
    std::wstring displayName;
    std::wstring token;
    std::wstring apiBase;
    // Server selected on the login screen. Net Goal 11 is open source and has
    // no license file, so the player simply types the LAN server address.
    std::wstring server;
    int port = 0;
};

// Shows the modal login window.  The window asks for the server IP/port plus a
// nickname and the shared access password; nothing is validated against a
// registration server.  Returns true only after login is accepted.
bool ShowLoginWindow(HINSTANCE instance, const std::wstring& defaultApiBase,
                     const std::wstring& username, const std::wstring& password,
                     LoginResult& result);
