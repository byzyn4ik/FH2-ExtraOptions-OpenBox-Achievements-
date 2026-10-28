// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rules service (see ach_service.h). Moved from the OpenBox fork of Extra
// Options (achievements\ach_dll.cpp, fh2-achievements.dll): the same state
// file, migration and pipe protocol, now inside the mod-loader plugin.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "ach_engine.h"
#include "ach_service.h"
#include "ach_store.h"

namespace ach {
namespace service {

namespace {

std::wstring pipe_name;
std::string producer;

CRITICAL_SECTION queue_lock, engine_lock, log_lock;
volatile LONG locks_ready;
std::string queue;              // telemetry not evaluated yet
HANDLE wake, flushed, thread;
volatile LONG running, stopping, flush_requested;
volatile LONG last_flush_ok;

std::wstring log_path, manifest_path, telemetry_copy;
std::string builtin_rules;
StorePaths store;
std::string game_id;

Engine engine;                  // engine_lock
bool ready;                     // rules and state loaded (worker only from here on)
bool dirty;
std::string carry;
unsigned long long manifest_stamp;
std::set<std::string> progress_dirty;
std::map<std::string, std::string> progress_sent;
bool hello_ok, full_sync = true, told_old;

void init_locks() {
    static volatile LONG once;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        InitializeCriticalSection(&queue_lock);
        InitializeCriticalSection(&engine_lock);
        InitializeCriticalSection(&log_lock);
        InterlockedExchange(&locks_ready, 1);
    }
    while (!InterlockedCompareExchange(&locks_ready, 0, 0)) Sleep(0);
}

void log_text(const char *text, size_t length) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02u  ", now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond);
    std::string line = stamp + std::string(text, length) + "\r\n";
    EnterCriticalSection(&log_lock);
    if (!log_path.empty()) {
        HANDLE file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, line.data(), (DWORD)line.size(), &written, nullptr);
            CloseHandle(file);
        }
    }
    LeaveCriticalSection(&log_lock);
}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void log_line(const char *format, ...) {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    int n = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (n < 0) return;
    if (n >= (int)sizeof(text)) n = (int)sizeof(text) - 1;
    log_text(text, (size_t)n);
}

std::string encode(const std::string &text) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (c <= 0x20 || c == '%' || c >= 0x7f) {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

std::string number_text(double value) {
    std::string out;
    json_number(out, value);
    return out;
}

// ------------------------------------------------------------- state files

bool apply_manifest(const std::string &text, const std::string &where) {
    std::string error;
    Manifest manifest;
    if (!manifest.parse(text, error)) {
        log_line("rules not read from %s (%s); %s", where.c_str(), error.c_str(),
                 ready ? "the previous rules stay" : "waiting for valid ones");
        return false;
    }
    EnterCriticalSection(&engine_lock);
    engine.manifest = std::move(manifest);
    LeaveCriticalSection(&engine_lock);
    log_line("%s rules: %zu rules for %zu achievements, from %s", manifest_stamp ? "reloaded" : "loaded",
             engine.manifest.rules.size(), engine.manifest.achievements.size(), where.c_str());
    full_sync = true;
    return true;
}

bool load_manifest(bool quiet_if_same) {
    if (manifest_path.empty()) {
        // The built-in rules never change while the game runs.
        if (quiet_if_same) return true;
        bool ok = apply_manifest(builtin_rules, "the plugin (built in)");
        manifest_stamp = 1;
        return ok;
    }
    unsigned long long stamp = file_stamp(manifest_path);
    if (!stamp) {
        if (!quiet_if_same) log_line("rules file not found: %s", narrow(manifest_path).c_str());
        return ready;
    }
    if (quiet_if_same && stamp == manifest_stamp) return true;
    std::string text;
    bool ok = read_file(manifest_path, text) && apply_manifest(text, narrow(manifest_path));
    unsigned long long previous = manifest_stamp;
    manifest_stamp = stamp; // retried when it changes again
    if (!ok && !previous) return false;
    return ok || ready;
}

bool save_state() {
    EnterCriticalSection(&engine_lock);
    std::string text = engine.state.to_json(game_id);
    LeaveCriticalSection(&engine_lock);
    if (!write_file_atomic(store.state, text)) return false;
    dirty = false;
    return true;
}

void note(const Result &result) {
    for (const auto &id : result.unlocked) log_line("UNLOCKED achievement %s", id.c_str());
    for (const auto &id : result.touched) progress_dirty.insert(id);
    if (result.changed || !result.unlocked.empty()) dirty = true;
}

bool load_state() {
    std::string text;
    if (read_file(store.state, text)) {
        State state;
        if (!state.parse(text, false, nullptr, nullptr)) {
            log_line("ingame_state.json is damaged; it is left as it is and nothing is recorded");
            return false;
        }
        Result result;
        std::string report;
        EnterCriticalSection(&engine_lock);
        bool had_cursor = state.events_cursor >= 0 || state.inbox_cursor >= 0;
        engine.state = std::move(state);
        catch_up(engine, store, result, report);
        LeaveCriticalSection(&engine_lock);
        log_line("state: %zu unlocks, %zu counters (%s)", engine.state.unlocked.size(),
                 engine.state.counters.size(), narrow(store.state).c_str());
        if (!report.empty()) log_line("%s", report.c_str());
        note(result);
        if (!had_cursor) dirty = true;
        if (dirty) save_state();
        return true;
    }
    Result result;
    std::string report;
    EnterCriticalSection(&engine_lock);
    migrate(engine, store, result, report);
    LeaveCriticalSection(&engine_lock);
    log_line("first run: %s", report.c_str());
    note(result);
    if (!save_state()) {
        log_line("cannot write %s", narrow(store.state).c_str());
        return false;
    }
    log_line("state saved to %s", narrow(store.state).c_str());
    return true;
}

// -------------------------------------------------------------------- pipe

enum Delivery { Delivered, AppAbsent, AppOld, DeliveryFailed };

Delivery deliver(const std::string &message) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) return AppOld; // a receive-only pipe: OpenBox before in-game rules
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipe_name.c_str(), 500);
        } else if (error == ERROR_FILE_NOT_FOUND) {
            if (attempt >= 2) return AppAbsent;
            Sleep(60); // between two connections OpenBox has no listener for a moment
        } else {
            return DeliveryFailed;
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) return AppAbsent;
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    size_t sent = 0;
    while (sent < message.size()) {
        DWORD written = 0;
        if (!WriteFile(pipe, message.data() + sent, (DWORD)(message.size() - sent), &written, nullptr) || !written) break;
        sent += written;
    }
    Delivery result = DeliveryFailed;
    if (sent == message.size()) {
        char reply[64];
        DWORD have = 0, got = 0;
        for (int waited = 0; waited < 3000; waited += 10) {
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &have, nullptr)) break;
            if (have) break;
            Sleep(10);
        }
        if (have && ReadFile(pipe, reply, sizeof(reply) - 1, &got, nullptr) && got >= 2 &&
            reply[0] == 'o' && reply[1] == 'k')
            result = Delivered;
    }
    CloseHandle(pipe);
    return result;
}

unsigned long long next_send;

bool send_updates(bool forced) {
    unsigned long long now = GetTickCount64();
    std::string message = "hello " + game_id + " 1 " + producer + "\n";
    std::set<std::string> unlocks;
    std::map<std::string, std::string> progress;
    EnterCriticalSection(&engine_lock);
    for (const auto &entry : engine.state.unlocked) {
        if (entry.second.notified) continue;
        unlocks.insert(entry.first);
        message += "unlocked " + game_id + " " + encode(entry.first) + " " + encode(entry.second.time) + "\n";
    }
    std::set<std::string> ids;
    if (full_sync) {
        for (const auto &rule : engine.manifest.rules) ids.insert(rule.achievement);
    } else {
        ids = progress_dirty;
    }
    for (const auto &id : ids) {
        double current, target;
        if (engine.state.unlocked.count(id) || !engine.progress(id, current, target)) continue;
        std::string value = number_text(current) + "/" + number_text(target);
        auto previous = progress_sent.find(id);
        if (!full_sync && previous != progress_sent.end() && previous->second == value) continue;
        progress[id] = value;
        message += "progress " + game_id + " " + encode(id) + " " + value + "\n";
    }
    LeaveCriticalSection(&engine_lock);
    if (hello_ok && !full_sync && unlocks.empty() && progress.empty()) {
        progress_dirty.clear();
        return true;
    }
    if (!forced && now < next_send) return false;
    message += "end\n";
    Delivery delivery = deliver(message);
    if (delivery != Delivered) {
        next_send = now + (delivery == AppOld ? 60000 : 3000);
        if (delivery == AppOld && !told_old) {
            told_old = true;
            log_line("OpenBox Achievements is running but too old to receive in-game unlocks; update it");
        }
        if (hello_ok) log_line("OpenBox Achievements not reachable; updates are kept and resent");
        hello_ok = false;
        full_sync = true;
        return false;
    }
    if (!hello_ok)
        log_line("OpenBox Achievements connected (%zu unlocks, %zu progress values sent)", unlocks.size(),
                 progress.size());
    for (const auto &id : unlocks) log_line("OpenBox notified: achievement %s", id.c_str());
    hello_ok = true;
    told_old = false;
    full_sync = false;
    next_send = now + 250;
    progress_dirty.clear();
    for (const auto &entry : progress) progress_sent[entry.first] = entry.second;
    EnterCriticalSection(&engine_lock);
    for (const auto &id : unlocks) {
        auto found = engine.state.unlocked.find(id);
        if (found != engine.state.unlocked.end() && !found->second.notified) {
            found->second.notified = true;
            dirty = true;
        }
    }
    LeaveCriticalSection(&engine_lock);
    return true;
}

// ------------------------------------------------------------------ worker

// [Logging] Telemetry=1: the records as the rules saw them, for replays.
void append_copy(const std::string &input) {
    HANDLE file = CreateFileW(telemetry_copy.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, input.data(), (DWORD)input.size(), &written, nullptr);
    CloseHandle(file);
}

DWORD WINAPI worker(void *) {
    unsigned long long last_init = 0, last_manifest = 0, last_save = 0, dropped_logged = 0;
    while (true) {
        WaitForSingleObject(wake, 1000);
        bool stop_now = stopping != 0;
        bool flush_now = InterlockedExchange(&flush_requested, 0) != 0;
        unsigned long long now = GetTickCount64();
        if (!ready) {
            if (now - last_init >= 5000 || last_init == 0 || flush_now || stop_now) {
                last_init = now;
                ready = load_manifest(false) && load_state();
            }
            if (!ready) {
                if (flush_now) {
                    InterlockedExchange(&last_flush_ok, 0);
                    SetEvent(flushed);
                }
                if (stop_now) break;
                continue;
            }
            last_manifest = now;
        }
        std::string input;
        EnterCriticalSection(&queue_lock);
        input.swap(queue);
        LeaveCriticalSection(&queue_lock);
        if (!input.empty()) {
            if (!telemetry_copy.empty()) append_copy(input);
            carry += input;
            Result result;
            EnterCriticalSection(&engine_lock);
            size_t used = engine.apply_buffer(carry.data(), carry.size(), result);
            LeaveCriticalSection(&engine_lock);
            carry.erase(0, used);
            if (carry.size() > (1u << 20)) carry.clear(); // never a line that long
            if (result.bad_lines && now - dropped_logged > 60000) {
                dropped_logged = now;
                log_line("%lld telemetry line(s) were not valid JSON and were skipped", result.bad_lines);
            }
            note(result);
            if (!result.unlocked.empty()) save_state();
        }
        if (now - last_manifest >= 3000) {
            last_manifest = now;
            load_manifest(true);
        }
        if (dirty && (now - last_save >= 1000 || flush_now || stop_now)) {
            last_save = now;
            if (!save_state()) log_line("cannot write %s; retrying", narrow(store.state).c_str());
        }
        bool delivered = send_updates(flush_now || stop_now);
        if (dirty && (flush_now || stop_now)) save_state();
        if (flush_now) {
            InterlockedExchange(&last_flush_ok, delivered && !dirty ? 1 : 0);
            SetEvent(flushed);
        }
        if (stop_now) break;
    }
    return 0;
}

std::wstring sanitize_game(const std::wstring &game) {
    std::wstring out;
    for (wchar_t c : game)
        out += ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' ||
                c == L'-')
                   ? c
                   : L'_';
    if (out.empty()) out = L"fh2";
    if (out.size() > 64) out.resize(64);
    return out;
}

} // namespace

void set_log(const std::wstring &path) {
    init_locks();
    EnterCriticalSection(&log_lock);
    log_path = path;
    LeaveCriticalSection(&log_lock);
}

void write_log(const char *format, ...) {
    init_locks();
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    int n = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (n < 0) return;
    if (n >= (int)sizeof(text)) n = (int)sizeof(text) - 1;
    log_text(text, (size_t)n);
}

bool start(const Options &options) {
    init_locks();
    if (InterlockedCompareExchange(&running, 1, 0)) return true;
    std::wstring id = sanitize_game(options.game);
    game_id = narrow(id);
    log_path = options.log;
    telemetry_copy = options.telemetry_copy;
    pipe_name = options.pipe;
    producer = options.producer;
    manifest_path = options.rules_file;
    builtin_rules = options.builtin_rules;
    store.set(options.data, id);
    engine.game = game_id;
    engine.clock = iso_now;
    log_line("rules service: game %s; rules from %s; state in %s; pipe %s", game_id.c_str(),
             manifest_path.empty() ? "the plugin (built in)" : narrow(manifest_path).c_str(),
             narrow(store.state).c_str(), narrow(pipe_name).c_str());
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    flushed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread = wake && flushed ? CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) : nullptr;
    if (!thread) {
        log_line("cannot start the worker thread");
        return false;
    }
    return true;
}

bool started() { return thread != nullptr; }

void submit(const char *bytes, size_t length) {
    if (!thread || !bytes || !length) return;
    EnterCriticalSection(&queue_lock);
    if (queue.size() + length <= (16u << 20)) queue.append(bytes, length);
    LeaveCriticalSection(&queue_lock);
    SetEvent(wake);
}

bool flush(unsigned timeout_ms) {
    if (!thread) return false;
    ResetEvent(flushed);
    InterlockedExchange(&flush_requested, 1);
    SetEvent(wake);
    return WaitForSingleObject(flushed, timeout_ms) == WAIT_OBJECT_0 && last_flush_ok;
}

void stop(unsigned timeout_ms) {
    if (!thread) return;
    InterlockedExchange(&stopping, 1);
    SetEvent(wake);
    WaitForSingleObject(thread, timeout_ms);
}

} // namespace service
} // namespace ach
