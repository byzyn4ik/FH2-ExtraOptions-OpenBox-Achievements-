// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the OpenBox fork of Forza Horizon 2 Extra Options
// (achievements/), which derives from Extra Options (GPL-3.0).

#include "ach_engine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ach {

namespace {

bool equal_ignore_case(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return false;
    }
    return true;
}

bool parse_int(const std::string &text, int &value) {
    // int.TryParse(Integer): optional white space and sign, then digits.
    size_t i = 0, n = text.size();
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    bool negative = false;
    if (i < n && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
    if (i >= n || text[i] < '0' || text[i] > '9') return false;
    long long result = 0;
    while (i < n && text[i] >= '0' && text[i] <= '9') {
        result = result * 10 + (text[i++] - '0');
        if (result > 2147483648LL) return false;
    }
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i != n) return false;
    if (negative) result = -result;
    if (result > 2147483647LL) return false;
    value = (int)result;
    return true;
}

std::string rule_key(const Rule &rule) {
    return rule.achievement + "|" + rule.event + "|" + rule.kind + "|" + std::to_string(rule.field);
}

const JValue *field_object(const std::vector<const JValue *> &fields, int index) {
    if (index < 0 || (size_t)index >= fields.size()) return nullptr;
    const JValue *field = fields[(size_t)index];
    return field && field->type == JValue::Object ? field : nullptr;
}

// Convert.ToString of what JavaScriptSerializer made of the value.
std::string value_text(const JValue *value) {
    if (!value) return "";
    switch (value->type) {
    case JValue::String: return value->text;
    case JValue::Number:
        if (value->text == "-0") return "0";
        return value->text;
    case JValue::Bool: return value->boolean ? "True" : "False";
    default: return "";
    }
}

double value_number(const JValue *value) {
    if (!value) return 0;
    switch (value->type) {
    case JValue::Number: return value->number;
    case JValue::Bool: return value->boolean ? 1 : 0;
    case JValue::String: {
        // OpenBox would throw on text that is not a number; here it counts as 0.
        char *stop = nullptr;
        double number = strtod(value->text.c_str(), &stop);
        return stop && stop != value->text.c_str() && *stop == 0 ? number : 0;
    }
    default: return 0;
    }
}

} // namespace

std::string field_text(const std::vector<const JValue *> &fields, int index) {
    const JValue *field = field_object(fields, index);
    return field ? value_text(field->get("v")) : "";
}

double field_number(const std::vector<const JValue *> &fields, int index) {
    const JValue *field = field_object(fields, index);
    if (!field) return 0;
    const JValue *type = field->get("t");
    const JValue *value = type && type->type == JValue::Number && type->number == 9 && field->get("f")
                              ? field->get("f")
                              : field->get("v");
    return value_number(value);
}

// ------------------------------------------------------------------ manifest

bool Manifest::parse(const std::string &text, std::string &error) {
    JValue root;
    size_t start = text.size() >= 3 && (unsigned char)text[0] == 0xef ? 3 : 0; // UTF-8 BOM
    if (!json_parse(text.data() + start, text.size() - start, root) || root.type != JValue::Object) {
        error = "the manifest is not valid JSON";
        return false;
    }
    Manifest result;
    if (const JValue *name_value = root.get("Name")) result.name = name_value->text;
    if (const JValue *list = root.get("Achievements"))
        for (const auto &item : list->items) result.achievements.push_back(value_text(&item));
    if (const JValue *list = root.get("Rules")) {
        for (const auto &item : list->items) {
            if (item.type != JValue::Object) continue;
            Rule rule;
            if (const JValue *v = item.get("Achievement")) rule.achievement = value_text(v);
            if (const JValue *v = item.get("Event")) rule.event = value_text(v);
            if (const JValue *v = item.get("Kind"); v && v->type == JValue::String) rule.kind = v->text;
            if (const JValue *v = item.get("Field"); v && v->is_number()) rule.field = (int)v->number;
            if (const JValue *v = item.get("Per"); v && v->is_number()) rule.per = (int)v->number;
            if (const JValue *v = item.get("Target"); v && v->is_number()) rule.target = v->number;
            if (const JValue *v = item.get("Distinct"); v && v->type == JValue::Bool) rule.distinct = v->boolean;
            if (const JValue *match = item.get("Match"); match && match->type == JValue::Object) {
                for (const auto &member : match->members) {
                    int index;
                    if (!parse_int(member.first, index)) rule.match_valid = false;
                    std::string pattern = value_text(&member.second);
                    bool negate = !pattern.empty() && pattern[0] == '!';
                    if (negate) pattern.erase(0, 1);
                    std::vector<std::string> options;
                    size_t from = 0;
                    while (true) {
                        size_t bar = pattern.find('|', from);
                        options.push_back(pattern.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
                        if (bar == std::string::npos) break;
                        from = bar + 1;
                    }
                    rule.match.emplace_back(index, std::move(options));
                    rule.match_not.push_back(negate);
                }
            }
            rule.key = rule_key(rule);
            result.by_event[rule.event].push_back(result.rules.size());
            result.rules.push_back(std::move(rule));
        }
    }
    *this = std::move(result);
    return true;
}

bool Manifest::has_achievement(const std::string &id) const {
    return std::find(achievements.begin(), achievements.end(), id) != achievements.end();
}

// --------------------------------------------------------------------- state

std::string State::to_json(const std::string &game) const {
    std::string out = "{\"Version\":1,\"Game\":";
    json_escape(out, game);
    out += ",\n\"Unlocked\":{";
    bool first = true;
    for (const auto &entry : unlocked) {
        out += first ? "\n" : ",\n";
        first = false;
        json_escape(out, entry.first);
        out += ":{\"Time\":";
        json_escape(out, entry.second.time);
        out += ",\"Source\":";
        json_escape(out, entry.second.source);
        out += entry.second.notified ? ",\"Notified\":true}" : ",\"Notified\":false}";
    }
    out += "},\n\"Counters\":{";
    first = true;
    for (const auto &entry : counters) {
        out += first ? "\n" : ",\n";
        first = false;
        json_escape(out, entry.first);
        out += ':';
        json_number(out, entry.second);
    }
    out += "}";
    if (events_cursor >= 0 || inbox_cursor >= 0) {
        out += ",\n\"RawCursor\":{\"Events\":";
        json_number(out, (double)events_cursor);
        out += ",\"Inbox\":";
        json_number(out, (double)inbox_cursor);
        out += "}";
    }
    if (!migrated_from.empty()) {
        out += ",\n\"Migration\":{\"From\":";
        json_escape(out, migrated_from);
        out += ",\"Time\":";
        json_escape(out, migrated_time);
        out += ",\"EventsOffset\":";
        json_number(out, (double)migrated_events_offset);
        out += ",\"InboxOffset\":";
        json_number(out, (double)migrated_inbox_offset);
        out += ",\"ReplayedLines\":";
        json_number(out, (double)replayed_lines);
        out += "}";
    }
    out += "\n}\n";
    return out;
}

bool State::parse(const std::string &text, bool openbox, long long *events_offset,
                  long long *inbox_offset) {
    JValue root;
    size_t start = text.size() >= 3 && (unsigned char)text[0] == 0xef ? 3 : 0;
    if (!json_parse(text.data() + start, text.size() - start, root) || root.type != JValue::Object)
        return false;
    State result;
    if (const JValue *list = root.get("Unlocked"); list && list->type == JValue::Object) {
        for (const auto &member : list->members) {
            Unlock unlock;
            if (const JValue *v = member.second.get("Time")) unlock.time = value_text(v);
            if (const JValue *v = member.second.get("Source"); v && v->is_string()) unlock.source = v->text;
            const JValue *notified = member.second.get("Notified");
            unlock.notified = openbox || !notified || notified->type != JValue::Bool || notified->boolean;
            result.unlocked[member.first] = unlock;
        }
    }
    if (const JValue *list = root.get("Counters"); list && list->type == JValue::Object)
        for (const auto &member : list->members)
            if (member.second.is_number()) result.counters[member.first] = member.second.number;
    if (const JValue *migration = root.get("Migration"); migration && migration->type == JValue::Object) {
        if (const JValue *v = migration->get("From")) result.migrated_from = value_text(v);
        if (const JValue *v = migration->get("Time")) result.migrated_time = value_text(v);
        if (const JValue *v = migration->get("EventsOffset"); v && v->is_number()) result.migrated_events_offset = (long long)v->number;
        if (const JValue *v = migration->get("InboxOffset"); v && v->is_number()) result.migrated_inbox_offset = (long long)v->number;
        if (const JValue *v = migration->get("ReplayedLines"); v && v->is_number()) result.replayed_lines = (long long)v->number;
    }
    if (const JValue *cursor = root.get("RawCursor"); cursor && cursor->type == JValue::Object) {
        if (const JValue *v = cursor->get("Events"); v && v->is_number()) result.events_cursor = (long long)v->number;
        if (const JValue *v = cursor->get("Inbox"); v && v->is_number()) result.inbox_cursor = (long long)v->number;
    }
    if (events_offset) {
        const JValue *v = root.get("EventsOffset");
        *events_offset = v && v->is_number() ? (long long)v->number : 0;
    }
    if (inbox_offset) {
        const JValue *v = root.get("InboxOffset");
        *inbox_offset = v && v->is_number() ? (long long)v->number : 0;
    }
    *this = std::move(result);
    return true;
}

// -------------------------------------------------------------------- engine

size_t Engine::apply_buffer(const char *data, size_t length, Result &result) {
    size_t consumed = 0;
    while (consumed < length) {
        const char *newline = (const char *)memchr(data + consumed, '\n', length - consumed);
        if (!newline) break;
        size_t line_end = (size_t)(newline - data);
        apply_line(data + consumed, line_end - consumed, result);
        consumed = line_end + 1;
    }
    return consumed;
}

void Engine::apply_line(const char *line, size_t length, Result &result) {
    while (length && (line[length - 1] == '\r' || line[length - 1] == ' ' || line[length - 1] == '\t')) --length;
    size_t start = 0;
    while (start < length && (line[start] == ' ' || line[start] == '\t')) ++start;
    if (start == length) return;
    ++result.lines;
    JValue record;
    if (!json_parse(line + start, length - start, record) || record.type != JValue::Object) {
        ++result.bad_lines;
        return;
    }
    const JValue *name = record.get("e"), *data = record.get("f");
    if (!name || !data) return;
    std::string event = name->type == JValue::String ? name->text : std::string();
    std::vector<const JValue *> fields;
    if (data->type == JValue::Array)
        for (const auto &item : data->items) fields.push_back(&item);
    if (game == "fh2") track_fh2_experience(event, fields, result);
    apply_event(event, fields, result);
}

// Cumulative XP, as OpenBox kept it: a save snapshot of the wristband level
// sets the baseline; each XPGained adds to it and counts as a new snapshot.
void Engine::track_fh2_experience(const std::string &name, const std::vector<const JValue *> &fields,
                                  Result &result) {
    static const std::string key = "telemetry|fh2|total-xp";
    if (name == "SaveStat" && field_text(fields, 3) == "CompareStats/WristbandLevel") {
        state.counters[key] = std::max(0.0, field_number(fields, 4));
        result.changed = true;
    } else if (name == "XPGained") {
        auto found = state.counters.find(key);
        if (found == state.counters.end()) return;
        double total = std::max(0.0, found->second + field_number(fields, 3));
        found->second = total;
        result.changed = true;
        JValue path, value;
        path.type = JValue::Object;
        path.members.emplace_back("t", JValue());
        path.members.back().second.type = JValue::Number;
        path.members.emplace_back("v", JValue());
        path.members.back().second.type = JValue::String;
        path.members.back().second.text = "CompareStats/WristbandLevel";
        value.type = JValue::Object;
        value.members.emplace_back("t", JValue());
        value.members.back().second.type = JValue::Number;
        value.members.back().second.number = 5;
        value.members.back().second.text = "5";
        value.members.emplace_back("v", JValue());
        value.members.back().second.type = JValue::Number;
        value.members.back().second.number = total;
        std::vector<const JValue *> snapshot = {nullptr, nullptr, nullptr, &path, &value};
        apply_event("SaveStat", snapshot, result);
    }
}

void Engine::apply_event(const std::string &name, const std::vector<const JValue *> &fields,
                         Result &result) {
    auto found = manifest.by_event.find(name);
    if (found == manifest.by_event.end()) return;
    for (size_t index : found->second) {
        const Rule &rule = manifest.rules[index];
        if (!rule.match_valid) continue;
        bool matched = true;
        for (size_t m = 0; m < rule.match.size() && matched; ++m) {
            std::string text = field_text(fields, rule.match[m].first);
            bool any = false;
            for (const auto &option : rule.match[m].second)
                if (equal_ignore_case(text, option)) { any = true; break; }
            matched = any != rule.match_not[m];
        }
        if (!matched) continue;
        auto existing = state.counters.find(rule.key);
        double value = existing == state.counters.end() ? 0 : existing->second;
        if (rule.per >= 0) {
            // e.g. events per car: each has its own count; the best is the progress.
            double &count = state.counters[rule.key + "|" + field_text(fields, rule.per)];
            count += 1;
            value = std::max(value, count);
        } else if (rule.distinct) {
            std::string seen = rule.key + "|" + field_text(fields, rule.field);
            if (state.counters.count(seen)) continue;
            state.counters[seen] = 1;
            value += 1;
        } else if (rule.kind == "max") {
            value = std::max(value, field_number(fields, rule.field));
        } else if (rule.kind == "sum") {
            value += field_number(fields, rule.field);
        } else {
            value += 1;
        }
        state.counters[rule.key] = value;
        result.changed = true;
        result.touched.insert(rule.achievement);
        reached(rule, value, result);
    }
}

void Engine::reached(const Rule &rule, double value, Result &result) {
    if (value < rule.target) return;
    // Unlocks by id; the manifest lists the game's achievements.
    if (!manifest.has_achievement(rule.achievement) || state.unlocked.count(rule.achievement)) return;
    Unlock unlock;
    unlock.time = clock ? clock() : std::string();
    unlock.source = "local";
    unlock.notified = false;
    state.unlocked[rule.achievement] = unlock;
    result.unlocked.push_back(rule.achievement);
}

bool Engine::progress(const std::string &id, double &current, double &target) const {
    double best = -1;
    for (const Rule &rule : manifest.rules) {
        if (rule.achievement != id || rule.target <= 1) continue;
        auto found = state.counters.find(rule.key);
        double value = found == state.counters.end() ? 0 : found->second;
        double fraction = std::min(1.0, value / rule.target);
        if (fraction <= best) continue;
        best = fraction;
        current = std::min(value, rule.target);
        target = rule.target;
    }
    return best >= 0;
}

} // namespace ach
