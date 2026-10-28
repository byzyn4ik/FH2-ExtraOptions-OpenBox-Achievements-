// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline test of the built plugin DLL, as the Extra Options loader drives it:
// a fake FH2 image (PE headers, the EtwPlus import, the ETX event table and
// the Achievements popup's vtable slot, laid out at the inspected RVAs), a fake
// OpenBox pipe server and a fake OpenBox app (this EXE, copied as
// "OpenBox Achievements.exe"). Never touches the real game or OpenBox: the
// runner (tests/run_tests.py) writes the plugin's INI with private paths and a
// private pipe name.
//
//   plugin_test <mode> <plugin dll> <pipe name>     modes: full, late, mismatch, noapp
//   "OpenBox Achievements.exe" --game ... | --background ...   (the fake app)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/fh2_plugin.h"

namespace {

constexpr uint32_t kImageSize = 0x3a18000u;
constexpr uint32_t kEtwIat = 0x1dd0050u; // EtxRegister, EtxUnregister, EtxEventWrite, EtxFillCommonFields_v7
constexpr uint32_t kEtxSlot = 0x1dd0060u;
constexpr uint32_t kEventTable = 0x2cb9280u;
constexpr uint32_t kFirstName = 0x1e64338u;
constexpr uint32_t kShowSlot = 0x2e360c8u;
constexpr uint32_t kShowMethod = 0xe38770u;

struct EventData {
    uint64_t pointer;
    uint32_t size, reserved;
};
typedef ULONG (*EtxWrite)(const void *, const void *, uint64_t, ULONG, const EventData *);

int failures;
std::wstring work;
volatile LONG original_calls;

void check(bool ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

ULONG fake_etx_write(const void *, const void *, uint64_t registration, ULONG, const EventData *) {
    InterlockedIncrement(&original_calls);
    return registration == 0x1234 ? 0 : 87;
}

std::string read_text(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    std::string text;
    char buffer[4096];
    DWORD got;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got) text.append(buffer, got);
    CloseHandle(file);
    return text;
}

void host_log(const char *message) {
    HANDLE file = CreateFileW((work + L"\\host.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, message, (DWORD)strlen(message), &written, nullptr);
    CloseHandle(file);
}

// ---------------------------------------------------------- fake OpenBox pipe

CRITICAL_SECTION received_lock;
std::string received;
std::wstring pipe_path;

DWORD WINAPI pipe_server(void *) {
    while (true) {
        HANDLE pipe = CreateNamedPipeW(pipe_path.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 4, 4096,
                                       4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return 1;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            continue;
        }
        std::string message;
        char buffer[4096];
        DWORD got;
        while (message.find("end\n") == std::string::npos && ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) &&
               got)
            message.append(buffer, got);
        DWORD written;
        WriteFile(pipe, "ok\n", 3, &written, nullptr);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        EnterCriticalSection(&received_lock);
        received += message;
        LeaveCriticalSection(&received_lock);
    }
}

// A line that starts with `line` (followed by a space or the line's end).
bool has_line(const std::string &text, const std::string &line) {
    for (size_t at = text.find(line); at != std::string::npos; at = text.find(line, at + 1)) {
        bool starts = at == 0 || text[at - 1] == '\n';
        char next = at + line.size() < text.size() ? text[at + line.size()] : 0;
        if (starts && (next == ' ' || next == '\n')) return true;
    }
    return false;
}

bool wait_received(const std::vector<std::string> &lines, unsigned timeout_ms) {
    for (unsigned waited = 0;; waited += 50) {
        EnterCriticalSection(&received_lock);
        bool all = true;
        for (const auto &line : lines) all = all && has_line(received, line);
        LeaveCriticalSection(&received_lock);
        if (all) return true;
        if (waited >= timeout_ms) return false;
        Sleep(50);
    }
}

bool was_received(const std::string &line) {
    EnterCriticalSection(&received_lock);
    bool found = has_line(received, line);
    LeaveCriticalSection(&received_lock);
    return found;
}

// ---------------------------------------------------------- fake game image

unsigned char *image;

template <typename T> void put(uint32_t rva, const T &value) { memcpy(image + rva, &value, sizeof(value)); }
void put_text(uint32_t rva, const char *text) { memcpy(image + rva, text, strlen(text) + 1); }

// Event descriptors, as the game's provider table has them.
const char *const kEvents[] = {"AnnaRecommendation", "CarPurchased", "SkillTokenEarned", "ChampionshipEnd",
                               "XPGained"};
constexpr uint32_t kNames = 0x1e70000u, kTypes = 0x1e71000u;

const void *descriptor(int index) { return image + kEventTable + 0x38u * (uint32_t)index; }

void build_image() {
    image = (unsigned char *)VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    // PE headers with one import: EtwPlus.dll.
    IMAGE_DOS_HEADER dos = {};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    put(0, dos);
    IMAGE_NT_HEADERS64 nt = {};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfImage = kImageSize;
    nt.OptionalHeader.NumberOfRvaAndSizes = 16;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {0x1000, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
    put(0x80, nt);
    IMAGE_IMPORT_DESCRIPTOR etw = {};
    etw.OriginalFirstThunk = 0x2000;
    etw.Name = 0x1100;
    etw.FirstThunk = kEtwIat;
    put(0x1000, etw);
    put_text(0x1100, "EtwPlus.dll");
    const char *functions[] = {"EtxRegister", "EtxUnregister", "EtxEventWrite", "EtxFillCommonFields_v7"};
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t hint = 0x3000 + 0x40 * i;
        put_text(hint + 2, functions[i]);
        put(0x2000 + 8 * i, (uint64_t)hint);
    }
    // The event table: names and field type codes.
    uint32_t name = kNames;
    for (int i = 0; i < (int)(sizeof(kEvents) / sizeof(kEvents[0])); ++i) {
        uint32_t at = i ? name : kFirstName;
        put_text(at, kEvents[i]);
        if (i) name += 0x40;
        put(kEventTable + 0x38u * (uint32_t)i + 0x10, image + at);
        put(kEventTable + 0x38u * (uint32_t)i + 0x20, image + kTypes);
    }
    // Types: three header fields (text), then 32-bit integers.
    const unsigned char types[8] = {0, 0, 0, 7, 7, 7, 7, 7};
    memcpy(image + kTypes, types, sizeof(types));
    // PopupShowAchievements' method: the guarded prologue, then
    // mov byte [rsi],1 (rsi = the page), and its epilogue.
    const unsigned char method[] = {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24,
                                    0x20, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x8b, 0xf1, 0xc6, 0x06, 0x01, 0x48,
                                    0x83, 0xc4, 0x40, 0x5f, 0x5e, 0x5b, 0xc3};
    memcpy(image + kShowMethod, method, sizeof(method));
    put(kShowSlot, image + kShowMethod);
    put(kEtxSlot, (void *)(uintptr_t)fake_etx_write);
}

void *slot_value(uint32_t rva) {
    void *value;
    memcpy(&value, image + rva, sizeof(value));
    return value;
}

// One event through the game's import slot, as the game calls it.
ULONG send_event(int index, const std::vector<int32_t> &values) {
    static const wchar_t empty[] = L"";
    std::vector<EventData> fields;
    for (int i = 0; i < 3; ++i) fields.push_back({(uint64_t)(uintptr_t)empty, sizeof(empty), 0});
    std::vector<int32_t> storage = values;
    for (auto &value : storage) fields.push_back({(uint64_t)(uintptr_t)&value, 4, 0});
    EtxWrite write = (EtxWrite)slot_value(kEtxSlot);
    return write(descriptor(index), nullptr, 0x1234, (ULONG)fields.size(), fields.data());
}

// -------------------------------------------------------------- the plugin

HMODULE plugin;
FH2PluginContext context;
std::wstring plugin_path, plugin_folder;

BOOL initialize() {
    plugin = LoadLibraryExW(plugin_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    check(plugin != nullptr, "the plugin DLL loads");
    if (!plugin) return FALSE;
    auto init = (FH2PluginInitializeFn)(uintptr_t)GetProcAddress(plugin, "FH2PluginInitialize");
    check(init != nullptr, "FH2PluginInitialize is exported");
    check(GetProcAddress(plugin, "FH2PluginVehicleUpdate") != nullptr, "FH2PluginVehicleUpdate is exported");
    if (!init) return FALSE;
    context.size = sizeof(context);
    context.api_version = FH2_PLUGIN_API_VERSION;
    context.adapter = FH2_PLUGIN_AZIMUTH;
    context.game_image_size = kImageSize;
    context.game_image = image;
    context.plugin_module = plugin;
    context.plugins_directory = plugin_folder.c_str();
    context.plugin_path = plugin_path.c_str();
    context.extra_options_version = "0.3.2";
    context.log_utf8 = host_log;
    return init(&context);
}

void vehicle_update() {
    auto update = (FH2PluginVehicleUpdateFn)(uintptr_t)GetProcAddress(plugin, "FH2PluginVehicleUpdate");
    static int car;
    FH2PluginVehicleFrame frame = {sizeof(frame), 1, &car, 0, 0.016f, nullptr};
    if (update) update(&frame);
}

std::wstring folder_of(const std::wstring &path) { return path.substr(0, path.find_last_of(L"\\/")); }

bool contains(const std::string &text, const char *what) { return text.find(what) != std::string::npos; }

int fake_app(int argc, wchar_t **argv) {
    // The plugin starts the app in its folder.
    std::string line;
    for (int i = 1; i < argc; ++i) {
        char text[512];
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, text, sizeof(text), nullptr, nullptr);
        if (n > 0) line += std::string(text) + " ";
    }
    line += "\r\n";
    HANDLE file = CreateFileW(L"launched.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, line.data(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(file);
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc >= 2 && (!wcscmp(argv[1], L"--game") || !wcscmp(argv[1], L"--background"))) return fake_app(argc, argv);
    if (argc < 4) {
        fprintf(stderr, "usage: plugin_test full|late|mismatch|noapp <plugin dll> <pipe name>\n");
        return 2;
    }
    std::wstring mode = argv[1];
    plugin_path = argv[2];
    plugin_folder = folder_of(plugin_path);
    work = folder_of(folder_of(folder_of(folder_of(plugin_path)))); // <work>\mods\<mod>\plugins\<dll>
    pipe_path = std::wstring(L"\\\\.\\pipe\\") + argv[3];
    InitializeCriticalSection(&received_lock);
    HANDLE server = CreateThread(nullptr, 0, pipe_server, nullptr, 0, nullptr);
    (void)server;
    build_image();
    printf("plugin test: %ls\n", mode.c_str());

    if (mode == L"late") put(kEtxSlot, (void *)nullptr); // the host has not resolved the import yet
    if (mode == L"mismatch") image[kFirstName] = 'X';   // not the inspected executable
    if (mode == L"badbutton") image[kShowMethod + 5] = 0x81; // a different prologue

    BOOL ready = initialize();
    check(ready == TRUE, "FH2PluginInitialize returns TRUE");
    std::string log = read_text(work + L"\\host.log");

    if (mode == L"noapp") {
        check(slot_value(kEtxSlot) == (void *)(uintptr_t)fake_etx_write, "no app: the ETX import is left alone");
        check(slot_value(kShowSlot) == image + kShowMethod, "no app: the Achievements button is left alone");
        check(contains(log, "is not installed"), "no app: the log says OpenBox is not installed");
        vehicle_update();
        return failures ? 1 : 0;
    }
    if (mode == L"mismatch") {
        check(slot_value(kEtxSlot) == (void *)(uintptr_t)fake_etx_write, "mismatch: the ETX import is left alone");
        check(contains(log, "the first ETX event name is not as expected"), "mismatch: the log names the guard");
        check(slot_value(kShowSlot) != image + kShowMethod, "mismatch: the (intact) button hook is still installed");
        vehicle_update();
        check(slot_value(kEtxSlot) == (void *)(uintptr_t)fake_etx_write, "mismatch: the vehicle update writes nothing");
        return failures ? 1 : 0;
    }
    if (mode == L"badbutton") {
        check(slot_value(kShowSlot) == image + kShowMethod, "badbutton: the Achievements button is left alone");
        check(contains(log, "the Achievements popup's method is not as expected"), "badbutton: the log names the guard");
        check(slot_value(kEtxSlot) != (void *)(uintptr_t)fake_etx_write, "badbutton: the ETX hook is still installed");
        return failures ? 1 : 0;
    }
    if (mode == L"late") {
        check(slot_value(kEtxSlot) == nullptr, "late: nothing is written to an unresolved slot");
        put(kEtxSlot, (void *)(uintptr_t)fake_etx_write); // the host fills the import
        bool wrapped = false;
        for (int i = 0; i < 60 && !wrapped; ++i) {
            Sleep(50);
            wrapped = slot_value(kEtxSlot) != (void *)(uintptr_t)fake_etx_write;
        }
        check(wrapped, "late: the import is wrapped once the host resolves it");
        // The host refills the slot with the same import: the vehicle update rewraps it.
        put(kEtxSlot, (void *)(uintptr_t)fake_etx_write);
        vehicle_update();
        check(slot_value(kEtxSlot) != (void *)(uintptr_t)fake_etx_write, "late: a refill is rewrapped");
        // Another module wraps it after us: left alone (no loop).
        static const unsigned char other[] = {0xc3};
        void *other_code = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(other_code, other, sizeof(other));
        put(kEtxSlot, other_code);
        vehicle_update();
        check(slot_value(kEtxSlot) == other_code, "late: a later wrapper by another module is left alone");
        return failures ? 1 : 0;
    }

    // full: telemetry, save stats, the pipe, the button and the app.
    check(slot_value(kEtxSlot) != (void *)(uintptr_t)fake_etx_write, "the ETX import slot is wrapped");
    check(slot_value(kShowSlot) != image + kShowMethod, "the Achievements button slot is hooked");
    check(contains(log, "EtxEventWrite wrapped"), "the host log reports the ETX hook");

    ULONG status = send_event(1, {});           // CarPurchased: achievement 27
    for (int i = 0; i < 3; ++i) send_event(2, {}); // SkillTokenEarned x3: 30 at 3/5
    send_event(3, {101});                        // ChampionshipEnd, distinct field 3
    send_event(3, {102});
    send_event(3, {102});                        // a repeat does not count
    check(status == 0, "the original EtxEventWrite's result is returned");
    check(original_calls == 7, "every event reaches the original EtxEventWrite");

    vehicle_update(); // first drive: the save stats are read on a worker
    vehicle_update();

    bool all = wait_received({"unlocked fh2 27", "progress fh2 30 3/5", "progress fh2 19 2/25", "unlocked fh2 28",
                              "progress fh2 29 60/175", "unlocked fh2 32"},
                             20000);
    if (!all) {
        EnterCriticalSection(&received_lock);
        printf("  received:\n%s\n", received.c_str());
        LeaveCriticalSection(&received_lock);
    }
    check(all, "OpenBox gets the unlocks (27, 28, 32) and progress (30 3/5, 19 2/25, 29 60/175)");
    check(was_received("hello fh2 1 fh2-eo-openbox-achievements/1.0.0"), "the pipe message starts with hello");

    // The pause menu's Achievements button: flush, the app, then the game's method.
    unsigned char page[64] = {};
    typedef void (*Show)(void *);
    ((Show)slot_value(kShowSlot))(page);
    check(page[0] == 1, "the game's own Achievements method still runs");
    std::string launched;
    for (int i = 0; i < 100; ++i) {
        launched = read_text(work + L"\\app\\launched.txt");
        if (contains(launched, "--game fh2")) break;
        Sleep(50);
    }
    check(contains(launched, "--game fh2"), "the button brings up OpenBox Achievements on FH2's page");
    HANDLE running = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\OpenBoxAchievements.Show");
    if (running) {
        CloseHandle(running);
        printf("  skip a real OpenBox Achievements is running: StartWithGame is not checked\n");
    } else {
        check(contains(launched, "--background --game fh2"), "the app is started in the tray with the game");
    }

    std::string state;
    for (int i = 0; i < 60; ++i) {
        state = read_text(work + L"\\data\\games\\fh2\\ingame_state.json");
        if (contains(state, "\"27\":{") && !contains(state, "\"Notified\":false")) break;
        Sleep(50);
    }
    check(contains(state, "\"27\":{") && contains(state, "\"28\":{") && contains(state, "\"32\":{"),
          "ingame_state.json has the unlocks");
    check(!contains(state, "\"Notified\":false"), "every unlock is marked as notified");
    return failures ? 1 : 0;
}
