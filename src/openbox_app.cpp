// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>

#include "openbox_app.h"

namespace openbox {

namespace {

bool absolute(const std::wstring &path) {
    return (path.size() > 1 && path[1] == L':') || (!path.empty() && (path[0] == L'\\' || path[0] == L'/'));
}

std::wstring trimmed(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool has_app(const std::wstring &folder) {
    DWORD attributes = GetFileAttributesW((folder + L"\\" + kAppName).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring registry_folder() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\OpenBox Achievements", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return std::wstring();
    std::wstring folder;
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, L"InstallFolder", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes > 0 && bytes < 65536) {
        std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, 0);
        if (RegQueryValueExW(key, L"InstallFolder", nullptr, &type, (BYTE *)value.data(), &bytes) == ERROR_SUCCESS)
            folder = value.data();
        if (type == REG_EXPAND_SZ && !folder.empty()) {
            wchar_t expanded[1024];
            DWORD n = ExpandEnvironmentStringsW(folder.c_str(), expanded, 1024);
            if (n && n <= 1024) folder = expanded;
        }
    }
    RegCloseKey(key);
    return folder;
}

} // namespace

std::wstring locate(const std::wstring &configured, const std::wstring &base, std::wstring *source) {
    if (!configured.empty()) {
        std::wstring folder = trimmed(absolute(configured) || base.empty() ? configured : base + L"\\" + configured);
        if (source) *source = L"[OpenBox] AppFolder";
        return has_app(folder) ? folder : std::wstring();
    }
    std::wstring folder = trimmed(registry_folder());
    if (source) *source = L"HKCU\\Software\\OpenBox Achievements\\InstallFolder";
    return !folder.empty() && has_app(folder) ? folder : std::wstring();
}

bool running() {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kShowEvent);
    if (!event) return false;
    CloseHandle(event);
    return true;
}

bool start(const std::wstring &folder, const std::wstring &arguments) {
    std::wstring exe = folder + L"\\" + kAppName;
    std::wstring command = L"\"" + exe + L"\"" + arguments;
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                        &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace openbox
