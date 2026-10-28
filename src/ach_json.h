// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the OpenBox fork of Forza Horizon 2 Extra Options
// (achievements/), which derives from Extra Options (GPL-3.0).

// Small JSON reader/writer for the plugin and its replay tool.
// Numbers keep their source text, so a rule's Match compares the text that
// OpenBox's JavaScriptSerializer would have produced (e.g. "1.500").
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace ach {

struct JValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string text; // String: the decoded UTF-8 text; Number: the source token
    std::vector<JValue> items;
    std::vector<std::pair<std::string, JValue>> members;

    const JValue *get(const char *key) const;
    bool is_number() const { return type == Number; }
    bool is_string() const { return type == String; }
};

// Parses one JSON document (leading/trailing white space allowed).
bool json_parse(const char *data, size_t size, JValue &out);

void json_escape(std::string &out, const std::string &text);
// Integral values print as integers; others with round-trip precision.
void json_number(std::string &out, double value);

} // namespace ach
