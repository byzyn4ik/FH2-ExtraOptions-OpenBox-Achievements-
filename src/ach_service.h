// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rules service: a worker thread that evaluates the FH2 achievement rules
// on telemetry records, keeps counters and unlocks in
// %LOCALAPPDATA%\OpenBoxAchievements\games\<game>\ingame_state.json and tells
// OpenBox Achievements only the results over its local pipe:
//
//   hello <game> 1 <producer>
//   unlocked <game> <id> <ISO-8601 UTC time>
//   progress <game> <id> <current>/<target>
//   end
//
// OpenBox answers "ok". Unlocks are resent until OpenBox has answered.
#pragma once
#include <string>

namespace ach {
namespace service {

struct Options {
    std::wstring game = L"fh2";
    std::wstring rules_file;     // empty: the rules built into the plugin
    std::string builtin_rules;   // the JSON used when rules_file is empty
    std::wstring data;           // %LOCALAPPDATA%\OpenBoxAchievements
    std::wstring log;            // the service log (empty: none)
    std::wstring telemetry_copy; // every record is also appended here (empty: none)
    std::wstring pipe = L"\\\\.\\pipe\\OpenBoxAchievements.Events.v1";
    std::string producer = "fh2-eo-openbox-achievements/1.0.0";
};

// Starts the worker once; later calls return true without changes.
bool start(const Options &options);
bool started();
// Complete telemetry lines ("\n"-terminated JSON records). Cheap: queued and
// evaluated on the worker thread.
void submit(const char *bytes, size_t length);
// Evaluates what is queued, saves the state and sends OpenBox what changed;
// waits up to timeout_ms. True when OpenBox has acknowledged everything.
bool flush(unsigned timeout_ms);
// A final evaluation, save and send; the worker ends (tests and tools).
void stop(unsigned timeout_ms);
// The service log's path, before start() (which sets it again).
void set_log(const std::wstring &path);
// One line in the service log.
void write_log(const char *format, ...);

} // namespace service
} // namespace ach
