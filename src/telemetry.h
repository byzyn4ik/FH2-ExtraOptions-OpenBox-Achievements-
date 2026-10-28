// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// FH2 reports gameplay to Xbox Live as ETX events (EtwPlus EtxEventWrite);
// the console's achievements were unlocked from them on the server. Each event
// becomes one JSON Lines record for the rules:
//   {"ms":<tick>,"e":"<event>","f":[{"t":<type>,"n":<size>,"v":<value>[,"f":<float>]},...]}
// the same records the OpenBox fork of Extra Options produced, so recorded
// logs replay unchanged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fh2 {

struct EventData {
    uint64_t pointer;
    uint32_t size, reserved;
};

// Inspected FH2 release (forza_durango_release_final.exe, SHA-256 3ec04c61...):
// the provider's 131 event descriptors start at 0x2cb9280, 0x38 bytes each:
// +0 the ETW descriptor, +0x10 the name, +0x18 the schema version, +0x20 the
// field type codes (one byte per field). The game passes the descriptor.
constexpr uint32_t kEventTable = 0x2cb9280u;
constexpr uint32_t kEventCount = 131u;
constexpr uint32_t kEventSize = 0x38u;
// The first descriptor's name, "AnnaRecommendation".
constexpr uint32_t kFirstEventName = 0x1e64338u;

// Reads memory that may be unmapped; false (and nothing copied) if it is.
bool safe_read(const void *address, void *out, size_t size);

// One record for the event, appended to `out` (with "\r\n").
void append_record(std::string &out, const unsigned char *image, const void *event, uint32_t count,
                   const EventData *data, unsigned long long milliseconds);

// Save stats. Azimuth writes each save as the folder
// <saves>\<player>\User_*\revisions\<n>\data, the revision's commit file last. ProfileData there is "ssmc", its size,
// the number of top-level entries, then entries: a 32-bit name length, the
// name, a space, four zero bytes, a 32-bit type and the value (15: a bag, its
// entry count then the entries; 3, 7: 32-bit; 4: 64-bit; 0, 1: a byte; 9: a
// float; 12: UTF-16 text after its length). Each integer stat becomes a
// SaveStat record: field 3 its path, e.g. "Island/RoadsDiscovered", field 4
// its value. The save has no mainland board count, so one more stat is added:
// "Mainland/BoardsSmashed", all boards less the island's (upgrade) boards.

// The newest complete revision's ProfileData among the save roots, by its
// commit file's time. False if none.
bool newest_save(const std::vector<std::wstring> &roots, std::wstring &file);
// Records for every integer stat of a ProfileData file's bytes; false when it
// is not the expected format (records already made stay in `out`).
bool save_stat_records(const unsigned char *bytes, size_t size, std::string &out,
                       unsigned long long milliseconds);

} // namespace fh2
