// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>

#include "game_hooks.h"

namespace fh2 {
namespace hooks {

namespace {

bool in_image(uint32_t rva, uint32_t length, uint32_t size) {
    return rva < size && length <= size - rva;
}

bool equal_ignore_case(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
        if (!x) return true;
    }
}

// A NUL-terminated name at an RVA, at most `capacity - 1` characters.
bool read_name(const unsigned char *image, uint32_t size, uint32_t rva, char *out, size_t capacity) {
    for (size_t i = 0; i < capacity; ++i) {
        if (!in_image(rva + (uint32_t)i, 1, size) || !safe_read(image + rva + i, &out[i], 1)) return false;
        if (!out[i]) return true;
    }
    return false;
}

bool is_code(const void *address) {
    MEMORY_BASIC_INFORMATION region;
    if (!address || VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return false;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return region.State == MEM_COMMIT && (region.Protect & executable) && !(region.Protect & PAGE_GUARD);
}

bool write_pointer(void **slot, void *expected, void *value) {
    DWORD previous, ignored;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) return false;
    bool swapped = InterlockedCompareExchangePointer(slot, value, expected) == expected;
    VirtualProtect(slot, sizeof(*slot), previous, &ignored);
    return swapped;
}

// The first bytes of 0xe38770 (push rbx/rsi/rdi; sub rsp,40h;
// mov qword [rsp+20h],-2; mov rsi,rcx), up to its first relative call.
const unsigned char kShowPrologue[] = {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7,
                                       0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x8b, 0xf1};
const char kFirstEvent[] = "AnnaRecommendation";

} // namespace

const char *check_event_table(const unsigned char *image, uint32_t size) {
    if (!image || size != kImageSize) return "the game image is not the supported size";
    char name[sizeof(kFirstEvent)];
    if (!safe_read(image + kFirstEventName, name, sizeof(name)) || memcmp(name, kFirstEvent, sizeof(name)))
        return "the first ETX event name is not as expected";
    const unsigned char *first = nullptr;
    if (!safe_read(image + kEventTable + 0x10, &first, sizeof(first)) || first != image + kFirstEventName)
        return "the ETX event table is not as expected";
    return nullptr;
}

const char *check_etx_import(const unsigned char *image, uint32_t size) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!image || size != kImageSize || !safe_read(image, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 64 || dos.e_lfanew > 4096 || !safe_read(image + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != size)
        return "the game image headers are not as expected";
    const IMAGE_DATA_DIRECTORY &directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !in_image(directory.VirtualAddress, directory.Size, size))
        return "the game has no readable import table";
    for (uint32_t index = 0; index < 512; ++index) {
        IMAGE_IMPORT_DESCRIPTOR descriptor;
        uint32_t at = directory.VirtualAddress + index * (uint32_t)sizeof(descriptor);
        if (!in_image(at, sizeof(descriptor), size) || !safe_read(image + at, &descriptor, sizeof(descriptor)))
            break;
        if (!descriptor.Name) break;
        char dll[64];
        if (!read_name(image, size, descriptor.Name, dll, sizeof(dll)) || !equal_ignore_case(dll, "EtwPlus.dll"))
            continue;
        uint32_t names = descriptor.OriginalFirstThunk;
        if (!names) return "EtwPlus.dll has no import name table";
        for (uint32_t i = 0; i < 4096; ++i) {
            uint64_t thunk;
            if (!in_image(names + 8 * i, 8, size) || !safe_read(image + names + 8 * i, &thunk, 8) || !thunk) break;
            if (thunk >> 63) continue; // by ordinal
            char function[32];
            if (!read_name(image, size, (uint32_t)thunk + 2, function, sizeof(function)) ||
                strcmp(function, "EtxEventWrite"))
                continue;
            return descriptor.FirstThunk + 8 * i == kEtxEventWriteSlot
                       ? nullptr
                       : "EtxEventWrite is not imported at the inspected slot";
        }
        return "EtwPlus.dll!EtxEventWrite is not imported";
    }
    return "EtwPlus.dll is not imported";
}

const char *check_achievements_button(const unsigned char *image, uint32_t size) {
    if (!image || size != kImageSize) return "the game image is not the supported size";
    const unsigned char *method = nullptr;
    if (!safe_read(image + kShowAchievementsSlot, &method, sizeof(method)) ||
        method != image + kShowAchievementsMethod)
        return "the Achievements popup's vtable slot is not as expected";
    unsigned char code[sizeof(kShowPrologue)];
    if (!safe_read(image + kShowAchievementsMethod, code, sizeof(code)) ||
        memcmp(code, kShowPrologue, sizeof(code)))
        return "the Achievements popup's method is not as expected";
    return nullptr;
}

// ------------------------------------------------------------------ ETX

namespace {

typedef ULONG (*EtxEventWrite)(const void *event, const void *provider, uint64_t registration, ULONG count,
                               const EventData *data);

unsigned char *etx_image;
void **etx_slot;
EventSink etx_sink;
EtxEventWrite volatile original_write;
void *first_original;
volatile LONG ensuring, installed_once;

ULONG event_write(const void *event, const void *provider, uint64_t registration, ULONG count,
                  const EventData *data) {
    EventSink sink = etx_sink;
    if (sink) sink(event, data ? count : 0, data);
    return original_write(event, provider, registration, count, data);
}

} // namespace

void etx_prepare(unsigned char *image, EventSink sink) {
    etx_image = image;
    etx_sink = sink;
    etx_slot = (void **)(image + kEtxEventWriteSlot);
}

bool etx_installed() { return InterlockedCompareExchange(&installed_once, 0, 0) != 0; }

EnsureResult etx_ensure() {
    if (!etx_slot) return EtxNotArmed;
    void *current = nullptr;
    if (!safe_read(etx_slot, &current, sizeof(current))) return EtxNotReady;
    void *wrapper = (void *)(uintptr_t)event_write;
    if (current == wrapper) return EtxAlreadyInstalled;
    if (!current) return EtxNotReady;
    if (InterlockedCompareExchange(&ensuring, 1, 0)) return EtxNotReady; // another thread is on it
    EnsureResult result;
    if (etx_installed() && current != first_original) {
        // Another module wrapped the import after us (and calls us): wrapping
        // it again would loop. Only a refill with the same import is rewrapped.
        result = EtxChained;
    } else if (!is_code(current) ||
               ((unsigned char *)current >= etx_image && (unsigned char *)current < etx_image + kImageSize)) {
        result = EtxNotCode; // not a resolved import (e.g. still the name's RVA)
    } else {
        original_write = (EtxEventWrite)current;
        if (write_pointer(etx_slot, current, wrapper)) {
            if (!first_original) first_original = current;
            InterlockedExchange(&installed_once, 1);
            result = EtxInstalled;
        } else {
            result = EtxNotReady;
        }
    }
    InterlockedExchange(&ensuring, 0);
    return result;
}

// ------------------------------------------------------- Achievements button

bool button_install(unsigned char *image, ShowMethod handler, ShowMethod *original) {
    if (check_achievements_button(image, kImageSize)) return false;
    void **slot = (void **)(image + kShowAchievementsSlot);
    void *method = image + kShowAchievementsMethod;
    *original = (ShowMethod)method;
    return write_pointer(slot, method, (void *)(uintptr_t)handler);
}

} // namespace hooks
} // namespace fh2
