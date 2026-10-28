// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from the OpenBox fork of Extra Options (features\achievement_events.c):
// the record format and the save-stat reader are unchanged.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>

#include "telemetry.h"

namespace fh2 {

bool safe_read(const void *address, void *out, size_t size) {
    if (!address || !out) return false;
    if (!size) return true;
    SIZE_T read = 0;
    // No exception on unmapped memory, unlike a plain copy.
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &read) && read == size;
}

namespace {

void put(std::string &line, const char *text, size_t length) { line.append(text, length); }
void put_text(std::string &line, const char *text) { line.append(text); }

void put_unsigned(std::string &line, uint64_t value) {
    char digits[24];
    unsigned count = 0;
    do {
        digits[sizeof(digits) - 1 - count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    put(line, digits + sizeof(digits) - count, count);
}

void put_signed(std::string &line, int64_t value) {
    if (value < 0) {
        put(line, "-", 1);
        put_unsigned(line, (uint64_t)0 - (uint64_t)value);
    } else {
        put_unsigned(line, (uint64_t)value);
    }
}

void put_hex(std::string &line, const unsigned char *data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    put(line, "\"", 1);
    for (size_t i = 0; i < size && i < 64; ++i) {
        char pair[2] = {digits[data[i] >> 4], digits[data[i] & 15]};
        put(line, pair, 2);
    }
    put(line, "\"", 1);
}

// Floats as fixed-point text with three decimals, as the fork wrote them.
void put_float(std::string &line, double value) {
    if (!(value == value) || value > 1e15 || value < -1e15) {
        put_text(line, "null");
        return;
    }
    if (value < 0) {
        put(line, "-", 1);
        value = -value;
    }
    uint64_t whole = (uint64_t)value;
    uint64_t fraction = (uint64_t)((value - (double)whole) * 1000 + 0.5);
    if (fraction >= 1000) {
        ++whole;
        fraction -= 1000;
    }
    put_unsigned(line, whole);
    char text[4] = {'.', (char)('0' + fraction / 100), (char)('0' + fraction / 10 % 10),
                    (char)('0' + fraction % 10)};
    put(line, text, 4);
}

// UTF-16 text as a JSON string (non-ASCII as \u escapes).
bool put_wide(std::string &line, const unsigned char *data, size_t size) {
    if (size < 2 || size % 2) return false;
    size_t count = size / 2;
    for (size_t i = 0; i < count; ++i) {
        uint16_t c = (uint16_t)(data[2 * i] | data[2 * i + 1] << 8);
        if (!c && i + 1 != count) return false;
        if (c && c < 32 && c != '\t') return false;
    }
    put(line, "\"", 1);
    for (size_t i = 0; i < count; ++i) {
        uint16_t c = (uint16_t)(data[2 * i] | data[2 * i + 1] << 8);
        if (!c) break;
        if (c == '"' || c == '\\') {
            char escaped[2] = {'\\', (char)c};
            put(line, escaped, 2);
        } else if (c < 128) {
            char plain = (char)c;
            put(line, &plain, 1);
        } else {
            static const char digits[] = "0123456789abcdef";
            char escaped[6] = {'\\', 'u', digits[c >> 12], digits[c >> 8 & 15], digits[c >> 4 & 15],
                               digits[c & 15]};
            put(line, escaped, 6);
        }
    }
    put(line, "\"", 1);
    return true;
}

// One field: its type code, size and the value read every way that fits.
void put_field(std::string &line, unsigned type, const EventData &field) {
    unsigned char data[64];
    size_t size = field.size < sizeof(data) ? field.size : sizeof(data);
    bool readable = field.pointer && safe_read((const void *)(uintptr_t)field.pointer, data, size);
    put_text(line, "{\"t\":");
    put_unsigned(line, type);
    put_text(line, ",\"n\":");
    put_unsigned(line, field.size);
    put_text(line, ",\"v\":");
    if (!readable) {
        put_text(line, "null}");
        return;
    }
    if (size == 1) {
        put_unsigned(line, data[0]);
    } else if (size == 2 && type != 0) {
        put_signed(line, (int16_t)(data[0] | data[1] << 8));
    } else if (size == 4) {
        int32_t integer;
        float number;
        memcpy(&integer, data, 4);
        memcpy(&number, data, 4);
        put_signed(line, integer);
        put_text(line, ",\"f\":");
        put_float(line, number);
    } else if (size == 8) {
        int64_t integer;
        double number;
        memcpy(&integer, data, 8);
        memcpy(&number, data, 8);
        put_signed(line, integer);
        put_text(line, ",\"f\":");
        put_float(line, number);
    } else if (field.size != size || !put_wide(line, data, size)) {
        put_hex(line, data, size);
    }
    put(line, "}", 1);
}

// A C string of the game, at most 63 characters, printable ASCII only.
void put_name(std::string &line, const char *name) {
    char text[64];
    size_t n = 0;
    bool whole = safe_read(name, text, sizeof(text)); // one read; byte by byte near a page end
    for (; n < sizeof(text) - 1; ++n) {
        if ((!whole && !safe_read(name + n, &text[n], 1)) || !text[n]) break;
        if (text[n] < 0x20 || text[n] > 0x7e || text[n] == '"' || text[n] == '\\') text[n] = '_';
    }
    put(line, text, n);
}

} // namespace

void append_record(std::string &line, const unsigned char *image, const void *event, uint32_t count,
                   const EventData *data, unsigned long long milliseconds) {
    const unsigned char *descriptor = (const unsigned char *)event;
    const unsigned char *table = image + kEventTable;
    const char *name = nullptr;
    const unsigned char *types = nullptr;
    uintptr_t offset = (uintptr_t)(descriptor - table);
    if (descriptor >= table && offset < kEventCount * kEventSize && offset % kEventSize == 0) {
        safe_read(descriptor + 0x10, &name, sizeof(name));
        safe_read(descriptor + 0x20, &types, sizeof(types));
    }
    put_text(line, "{\"ms\":");
    put_unsigned(line, milliseconds);
    put_text(line, ",\"e\":\"");
    if (name) {
        put_name(line, name);
    } else {
        put_text(line, "?");
    }
    put_text(line, "\",\"f\":[");
    unsigned char type_codes[64];
    uint32_t fields = data ? (count < 64 ? count : 64) : 0;
    bool have_types = types && fields && safe_read(types, type_codes, fields);
    for (uint32_t i = 0; i < fields; ++i) {
        unsigned char type = 0xff;
        if (have_types) {
            type = type_codes[i];
        } else if (types) {
            safe_read(types + i, &type, 1);
        }
        if (i) put(line, ",", 1);
        EventData field = {0, 0, 0};
        safe_read(&data[i], &field, sizeof(field));
        put_field(line, type, field);
    }
    put_text(line, "]}\r\n");
}

// ---------------------------------------------------------------- save stats

namespace {

constexpr size_t kStatName = 96;

struct StatReader {
    const unsigned char *bytes;
    size_t size;
    std::string &out;
    unsigned long long milliseconds;
    int64_t all_boards = 0, island_boards = 0;

    bool u32(size_t offset, uint32_t &value) const {
        if (offset + 4 > size || offset + 4 < offset) return false;
        memcpy(&value, bytes + offset, 4);
        return true;
    }

    void put_stat(const char *name, size_t length, int64_t value) {
        put_text(out, "{\"ms\":");
        put_unsigned(out, milliseconds);
        // The game's events start with three header fields; these are empty.
        put_text(out, ",\"e\":\"SaveStat\",\"f\":[{\"t\":0,\"n\":0,\"v\":\"\"},"
                      "{\"t\":0,\"n\":0,\"v\":\"\"},{\"t\":0,\"n\":0,\"v\":\"\"},{\"t\":0,\"n\":");
        put_unsigned(out, length);
        put_text(out, ",\"v\":\"");
        for (size_t i = 0; i < length; ++i) {
            char c = name[i];
            put(out, (c < 0x20 || c > 0x7e || c == '"' || c == '\\') ? "_" : &c, 1);
        }
        put_text(out, "\"},{\"t\":5,\"n\":8,\"v\":");
        put_signed(out, value);
        put_text(out, "}]}\r\n");
    }

    static bool is(const char *name, size_t length, const char *wanted) {
        return strlen(wanted) == length && !memcmp(name, wanted, length);
    }

    bool entry(size_t &offset, char *path, size_t path_length, unsigned depth) {
        uint32_t length, type, count;
        if (depth > 8 || !u32(offset, length) || !length || length > 64) return false;
        size_t name = offset + 4, n = path_length;
        offset = name + length + 1 + 4; // the name, a space, four zero bytes
        if (!u32(offset, type) || n + 1 + length >= kStatName) return false;
        offset += 4;
        if (n) path[n++] = '/';
        memcpy(path + n, bytes + name, length);
        n += length;
        int64_t value = 0;
        switch (type) {
        case 15:
            if (!u32(offset, count)) return false;
            offset += 4;
            for (uint32_t i = 0; i < count; ++i)
                if (!entry(offset, path, n, depth + 1)) return false;
            return true;
        case 12:
            if (!u32(offset, count)) return false;
            offset += 4 + 2 * (size_t)count;
            return offset <= size;
        case 9: // floats are settings and telemetry
            offset += 4;
            return offset <= size;
        case 0:
        case 1:
            if (offset + 1 > size) return false;
            value = bytes[offset];
            offset += 1;
            break;
        case 3:
        case 7: {
            int32_t integer;
            if (offset + 4 > size) return false;
            memcpy(&integer, bytes + offset, 4);
            value = integer;
            offset += 4;
            break;
        }
        case 4:
            if (offset + 8 > size) return false;
            memcpy(&value, bytes + offset, 8);
            offset += 8;
            break;
        default:
            return false;
        }
        put_stat(path, n, value);
        if (is(path, n, "CompareStats/DiscountBoardsSmashed")) {
            all_boards = value;
        } else if (is(path, n, "MiscStats/UpgradesBoardsSmashed")) {
            island_boards = value;
        }
        return true;
    }
};

bool is_folder(const WIN32_FIND_DATAW &found) {
    return (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(found.cFileName[0] == L'.' &&
             (!found.cFileName[1] || (found.cFileName[1] == L'.' && !found.cFileName[2])));
}

template <typename Visit> void each_folder(const std::wstring &parent, const wchar_t *pattern, Visit visit) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((parent + L"\\" + pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (is_folder(found)) visit(parent + L"\\" + found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

} // namespace

bool newest_save(const std::vector<std::wstring> &roots, std::wstring &file) {
    uint64_t newest = 0;
    bool found = false;
    for (const auto &root : roots) {
        if (root.empty()) continue;
        each_folder(root, L"*", [&](const std::wstring &player) {
            each_folder(player, L"User_*", [&](const std::wstring &user) {
                each_folder(user + L"\\revisions", L"*", [&](const std::wstring &revision) {
                    WIN32_FILE_ATTRIBUTE_DATA commit;
                    if (!GetFileAttributesExW((revision + L"\\commit").c_str(), GetFileExInfoStandard, &commit))
                        return;
                    uint64_t time = (uint64_t)commit.ftLastWriteTime.dwHighDateTime << 32 |
                                    commit.ftLastWriteTime.dwLowDateTime;
                    if (!found || time > newest) {
                        newest = time;
                        found = true;
                        file = revision + L"\\data\\ProfileData";
                    }
                });
            });
        });
    }
    return found;
}

bool save_stat_records(const unsigned char *bytes, size_t size, std::string &out, unsigned long long milliseconds) {
    StatReader reader{bytes, size, out, milliseconds};
    uint32_t count;
    if (size < 12 || memcmp(bytes, "ssmc", 4) || !reader.u32(8, count)) return false;
    size_t offset = 12;
    char path[kStatName];
    for (uint32_t i = 0; i < count; ++i)
        if (!reader.entry(offset, path, 0, 0)) break; // a format we don't know: what was read so far stands
    static const char mainland[] = "Mainland/BoardsSmashed";
    reader.put_stat(mainland, sizeof(mainland) - 1, reader.all_boards - reader.island_boards);
    return true;
}

} // namespace fh2
