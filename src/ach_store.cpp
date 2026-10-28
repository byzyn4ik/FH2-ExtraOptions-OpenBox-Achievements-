// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the OpenBox fork of Forza Horizon 2 Extra Options
// (achievements/), which derives from Extra Options (GPL-3.0).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ach_store.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ach {

void StorePaths::set(const std::wstring &data_folder, const std::wstring &game_id) {
    data = data_folder;
    game = game_id;
    std::wstring local = data + L"\\games\\" + game;
    state = local + L"\\ingame_state.json";
    openbox = local + L"\\state.json";
    events = data + L"\\events\\" + game + L".jsonl";
    inbox = data + L"\\inbox\\" + game + L".jsonl";
}

std::string narrow(const std::wstring &text) {
    if (text.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string &text) {
    if (text.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], n);
    return out;
}

bool read_file(const std::wstring &path, std::string &out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (64LL << 20);
    if (ok) {
        out.assign((size_t)size.QuadPart, '\0');
        DWORD read = 0;
        ok = size.QuadPart == 0 || (ReadFile(file, &out[0], (DWORD)size.QuadPart, &read, nullptr) &&
                                    read == (DWORD)size.QuadPart);
    }
    CloseHandle(file);
    return ok;
}

void create_folders(const std::wstring &folder) {
    for (size_t i = 3; i <= folder.size(); ++i)
        if (i == folder.size() || folder[i] == L'\\' || folder[i] == L'/')
            CreateDirectoryW(folder.substr(0, i).c_str(), nullptr);
}

bool write_file_atomic(const std::wstring &path, const std::string &data) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) create_folders(path.substr(0, slash));
    std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    ok = FlushFileBuffers(file) && ok;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

unsigned long long file_stamp(const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    unsigned long long time = (unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32 | data.ftLastWriteTime.dwLowDateTime;
    return time ^ ((unsigned long long)data.nFileSizeLow * 0x9e3779b97f4a7c15ULL) ^ data.nFileSizeHigh;
}

bool file_exists(const std::wstring &path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::string iso_now() {
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    SYSTEMTIME s;
    FileTimeToSystemTime(&time, &s);
    unsigned long long ticks = ((unsigned long long)time.dwHighDateTime << 32 | time.dwLowDateTime) % 10000000ULL;
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%07lluZ", s.wYear, s.wMonth, s.wDay,
             s.wHour, s.wMinute, s.wSecond, ticks);
    return buffer;
}

long long replay_file(Engine &engine, const std::wstring &path, long long offset, Result &result) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return offset;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return offset;
    }
    if (size.QuadPart < offset) offset = 0;
    std::vector<char> buffer(1 << 20);
    std::string carry;
    long long position = offset;
    LARGE_INTEGER seek;
    seek.QuadPart = offset;
    SetFilePointerEx(file, seek, nullptr, FILE_BEGIN);
    while (position < size.QuadPart) {
        DWORD want = (DWORD)std::min<long long>((long long)buffer.size(), size.QuadPart - position), read = 0;
        if (!ReadFile(file, buffer.data(), want, &read, nullptr) || !read) break;
        position += read;
        carry.append(buffer.data(), read);
        size_t used = engine.apply_buffer(carry.data(), carry.size(), result);
        offset += (long long)used;
        carry.erase(0, used);
    }
    CloseHandle(file);
    return offset;
}

bool migrate(Engine &engine, const StorePaths &paths, Result &result, std::string &report) {
    std::string text;
    long long events_offset = 0, inbox_offset = 0;
    bool found = read_file(paths.openbox, text) &&
                 engine.state.parse(text, true, &events_offset, &inbox_offset);
    if (!found) engine.state = State();
    State &state = engine.state;
    size_t counters = state.counters.size(), unlocks = state.unlocked.size();
    long long lines_before = result.lines;
    long long events_end = replay_file(engine, paths.events, events_offset, result);
    long long inbox_end = replay_file(engine, paths.inbox, inbox_offset, result);
    state.migrated_from = found ? narrow(paths.openbox) : std::string("(none: new progress)");
    state.migrated_time = iso_now();
    state.migrated_events_offset = events_offset;
    state.migrated_inbox_offset = inbox_offset;
    state.replayed_lines = result.lines - lines_before;
    state.events_cursor = events_end;
    state.inbox_cursor = inbox_end;
    char line[512];
    snprintf(line, sizeof(line),
             "%s: %zu unlocks, %zu counters; replayed %lld telemetry lines OpenBox had not read "
             "(events %lld..%lld, inbox %lld..%lld), %zu new unlocks.",
             found ? "migrated OpenBox progress" : "no OpenBox progress found", unlocks, counters,
             state.replayed_lines, events_offset, events_end, inbox_offset, inbox_end, result.unlocked.size());
    report = line;
    return found;
}

long long catch_up(Engine &engine, const StorePaths &paths, Result &result, std::string &report) {
    State &state = engine.state;
    long long before = result.lines;
    long long events_from = state.events_cursor < 0 ? 0 : state.events_cursor;
    long long inbox_from = state.inbox_cursor < 0 ? 0 : state.inbox_cursor;
    if (state.events_cursor < 0 && state.inbox_cursor < 0) {
        // A state from before cursors: start at the files' current ends.
        Engine idle;
        Result ignored;
        state.events_cursor = replay_file(idle, paths.events, 0, ignored);
        state.inbox_cursor = replay_file(idle, paths.inbox, 0, ignored);
        report.clear();
        return 0;
    }
    state.events_cursor = replay_file(engine, paths.events, events_from, result);
    state.inbox_cursor = replay_file(engine, paths.inbox, inbox_from, result);
    long long lines = result.lines - before;
    char line[256];
    snprintf(line, sizeof(line),
             "replayed %lld raw telemetry lines written without the DLL (events %lld..%lld, inbox %lld..%lld), %zu new unlocks",
             lines, events_from, state.events_cursor, inbox_from, state.inbox_cursor, result.unlocked.size());
    report = lines ? line : "";
    return lines;
}

} // namespace ach
