// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the OpenBox fork of Forza Horizon 2 Extra Options
// (achievements/), which derives from Extra Options (GPL-3.0).

// FH2 achievement rules, evaluated in game (the plugin).
//
// The manifest is OpenBox Achievements' games\<game>\achievement_manifest.json
// (see its docs\MANIFEST.md). Telemetry lines are the mod's JSON Lines
// records, {"e":"<event>","f":[{"t":type,"v":value,"f":float},...]}, the same
// lines OpenBox used to read from its inbox. Counters are keyed exactly as
// OpenBox keyed them in state.json ("<achievement>|<event>|<kind>|<field>"
// plus "|<value>" for Distinct/Per), so progress migrates one to one.
#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ach_json.h"

namespace ach {

struct Rule {
    std::string achievement, event, kind = "count";
    int field = -1, per = -1;
    double target = 1;
    bool distinct = false;
    bool match_valid = true; // a Match key that is not a number never matches
    std::vector<std::pair<int, std::vector<std::string>>> match; // field -> alternatives
    std::vector<bool> match_not;                                 // "!a|b"
    std::string key;
};

struct Manifest {
    std::string name;
    std::vector<std::string> achievements;
    std::vector<Rule> rules;
    std::unordered_map<std::string, std::vector<size_t>> by_event;

    bool parse(const std::string &text, std::string &error);
    bool has_achievement(const std::string &id) const;
};

struct Unlock {
    std::string time, source = "local";
    bool notified = true; // OpenBox acknowledged it (false: still to send)
};

struct State {
    std::map<std::string, double> counters;
    std::map<std::string, Unlock> unlocked;
    // Where the progress came from on first run (kept for the record).
    std::string migrated_from, migrated_time;
    long long migrated_events_offset = -1, migrated_inbox_offset = -1;
    long long replayed_lines = 0;
    // How far this DLL has read OpenBox's raw telemetry files (-1: not yet).
    // Only a build without the DLL writes them; the DLL replays what it missed.
    long long events_cursor = -1, inbox_cursor = -1;

    std::string to_json(const std::string &game) const;
    // Reads this DLL's state file, or OpenBox's state.json (openbox = true:
    // its unlocks count as already notified; offsets are returned).
    bool parse(const std::string &text, bool openbox, long long *events_offset,
               long long *inbox_offset);
};

struct Result {
    std::vector<std::string> unlocked;   // newly unlocked, in order
    std::set<std::string> touched;       // achievements whose counters changed
    bool changed = false;
    long long lines = 0, bad_lines = 0;
};

class Engine {
  public:
    std::string game = "fh2";
    Manifest manifest;
    State state;
    std::function<std::string()> clock; // ISO 8601 UTC for unlock times

    // One telemetry record (a line without its newline).
    void apply_line(const char *line, size_t length, Result &result);
    // Complete lines of a buffer; returns the bytes consumed (up to the last '\n').
    size_t apply_buffer(const char *data, size_t length, Result &result);

    // Viewer progress for an achievement, as OpenBox computed it: the rule
    // furthest along among its rules with Target > 1. False without one.
    bool progress(const std::string &id, double &current, double &target) const;

  private:
    void apply_event(const std::string &name, const std::vector<const JValue *> &fields,
                     Result &result);
    void track_fh2_experience(const std::string &name, const std::vector<const JValue *> &fields,
                              Result &result);
    void reached(const Rule &rule, double value, Result &result);
};

std::string field_text(const std::vector<const JValue *> &fields, int index);
double field_number(const std::vector<const JValue *> &fields, int index);

} // namespace ach
