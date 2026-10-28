// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the OpenBox fork of Forza Horizon 2 Extra Options
// (achievements/), which derives from Extra Options (GPL-3.0).

// Files of the in-game achievements: the DLL's own state, and the one-time
// migration of OpenBox Achievements' progress. Windows only (wide paths).
#pragma once
#include <string>

#include "ach_engine.h"

namespace ach {

struct StorePaths {
    std::wstring data;      // %LOCALAPPDATA%\OpenBoxAchievements
    std::wstring game;      // e.g. fh2
    std::wstring state;     // data\games\<game>\ingame_state.json  (this DLL's own)
    std::wstring openbox;   // data\games\<game>\state.json         (OpenBox's, read only)
    std::wstring events;    // data\events\<game>.jsonl             (old raw telemetry, read only)
    std::wstring inbox;     // data\inbox\<game>.jsonl              (raw telemetry, read only)
    void set(const std::wstring &data_folder, const std::wstring &game_id);
};

bool read_file(const std::wstring &path, std::string &out);
bool write_file_atomic(const std::wstring &path, const std::string &data);
unsigned long long file_stamp(const std::wstring &path); // last write time and size; 0 if missing
bool file_exists(const std::wstring &path);
void create_folders(const std::wstring &folder);
std::string iso_now();
std::string narrow(const std::wstring &text);
std::wstring widen(const std::string &text);

// Replays complete lines of a telemetry file from offset (0 if the file is
// shorter, as OpenBox did); returns the offset after the last complete line.
long long replay_file(Engine &engine, const std::wstring &path, long long offset, Result &result);

// First run: OpenBox's counters and unlocks become this DLL's state, then the
// telemetry OpenBox had not read yet (beyond its saved cursors) is replayed.
// Nothing of OpenBox's is written. Returns false if there was nothing to
// migrate (a fresh state, with the whole inbox replayed).
bool migrate(Engine &engine, const StorePaths &paths, Result &result, std::string &report);

// Later runs: raw telemetry a build without the DLL queued in OpenBox's inbox
// (or events file) after the DLL's cursors is replayed once, so the in-game
// state misses nothing. The mod never writes there while the DLL runs.
// Returns the lines replayed.
long long catch_up(Engine &engine, const StorePaths &paths, Result &result, std::string &report);

} // namespace ach
