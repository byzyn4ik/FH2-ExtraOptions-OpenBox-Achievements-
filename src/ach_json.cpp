// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the OpenBox fork of Forza Horizon 2 Extra Options
// (achievements/), which derives from Extra Options (GPL-3.0).

#include "ach_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ach {

const JValue *JValue::get(const char *key) const {
    if (type != Object) return nullptr;
    for (const auto &member : members)
        if (member.first == key) return &member.second;
    return nullptr;
}

namespace {

struct Parser {
    const char *p, *end;
    int depth = 0;

    void space() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }

    static void put_utf8(std::string &out, unsigned code) {
        if (code < 0x80) {
            out += (char)code;
        } else if (code < 0x800) {
            out += (char)(0xc0 | code >> 6);
            out += (char)(0x80 | (code & 0x3f));
        } else if (code < 0x10000) {
            out += (char)(0xe0 | code >> 12);
            out += (char)(0x80 | (code >> 6 & 0x3f));
            out += (char)(0x80 | (code & 0x3f));
        } else {
            out += (char)(0xf0 | code >> 18);
            out += (char)(0x80 | (code >> 12 & 0x3f));
            out += (char)(0x80 | (code >> 6 & 0x3f));
            out += (char)(0x80 | (code & 0x3f));
        }
    }

    bool hex4(unsigned &value) {
        if (end - p < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            value <<= 4;
            if (c >= '0' && c <= '9') value |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') value |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string &out) {
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= end) return false;
            c = *p++;
            switch (c) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code;
                if (!hex4(code)) return false;
                // A surrogate pair becomes one code point; a lone surrogate
                // (the mod writes raw UTF-16 GUID bytes) is kept as is.
                if (code >= 0xd800 && code < 0xdc00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    const char *save = p;
                    p += 2;
                    unsigned low;
                    if (hex4(low) && low >= 0xdc00 && low < 0xe000) {
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    } else {
                        p = save;
                    }
                }
                put_utf8(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool value(JValue &out) {
        space();
        if (p >= end || ++depth > 64) return false;
        bool ok = false;
        char c = *p;
        if (c == '{') {
            out.type = JValue::Object;
            ++p;
            space();
            if (p < end && *p == '}') {
                ++p;
                ok = true;
            } else {
                while (true) {
                    space();
                    std::string key;
                    if (!string(key)) break;
                    space();
                    if (p >= end || *p != ':') break;
                    ++p;
                    out.members.emplace_back(std::move(key), JValue());
                    if (!value(out.members.back().second)) break;
                    space();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; ok = true; }
                    break;
                }
            }
        } else if (c == '[') {
            out.type = JValue::Array;
            ++p;
            space();
            if (p < end && *p == ']') {
                ++p;
                ok = true;
            } else {
                while (true) {
                    out.items.emplace_back();
                    if (!value(out.items.back())) break;
                    space();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; ok = true; }
                    break;
                }
            }
        } else if (c == '"') {
            out.type = JValue::String;
            ok = string(out.text);
        } else if (c == 't' && end - p >= 4 && !memcmp(p, "true", 4)) {
            out.type = JValue::Bool;
            out.boolean = true;
            p += 4;
            ok = true;
        } else if (c == 'f' && end - p >= 5 && !memcmp(p, "false", 5)) {
            out.type = JValue::Bool;
            p += 5;
            ok = true;
        } else if (c == 'n' && end - p >= 4 && !memcmp(p, "null", 4)) {
            p += 4;
            ok = true;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char *start = p;
            if (*p == '-') ++p;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                               *p == '+' || *p == '-'))
                ++p;
            out.type = JValue::Number;
            out.text.assign(start, (size_t)(p - start));
            char *stop = nullptr;
            out.number = strtod(out.text.c_str(), &stop);
            ok = stop && *stop == 0 && out.text != "-";
        }
        --depth;
        return ok;
    }
};

} // namespace

bool json_parse(const char *data, size_t size, JValue &out) {
    Parser parser{data, data + size};
    out = JValue();
    if (!parser.value(out)) return false;
    parser.space();
    return parser.p == parser.end;
}

void json_escape(std::string &out, const std::string &text) {
    static const char digits[] = "0123456789abcdef";
    out += '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20) {
            out += "\\u00";
            out += digits[c >> 4];
            out += digits[c & 15];
        } else {
            out += (char)c;
        }
    }
    out += '"';
}

void json_number(std::string &out, double value) {
    char buffer[40];
    if (!std::isfinite(value)) {
        out += "0";
        return;
    }
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    } else {
        snprintf(buffer, sizeof(buffer), "%.17g", value);
    }
    out += buffer;
}

} // namespace ach
