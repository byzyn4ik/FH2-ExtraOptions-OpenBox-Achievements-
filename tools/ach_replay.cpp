// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the OpenBox fork of Forza Horizon 2 Extra Options
// (achievements/), which derives from Extra Options (GPL-3.0).

// Offline checks for the in-game achievement rules (never touches live data
// unless given its paths; it only writes --out).
//
//   ach-replay replay  --manifest M --out S.json [--game fh2] FILE...
//       evaluates the telemetry files, in order, from no progress
//   ach-replay migrate --manifest M --data DIR --out S.json [--game fh2]
//       the DLL's first run: DIR\games\<game>\state.json plus the unread
//       tails of DIR\events\<game>.jsonl and DIR\inbox\<game>.jsonl
//   ach-replay compare EXPECTED.json ACTUAL.json
//       counters and unlocks (either file may be OpenBox's state.json);
//       exit code 0 when they are equal
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ach_engine.h"
#include "ach_store.h"

using namespace ach;

static std::wstring argument(int argc, wchar_t **argv, const wchar_t *name, const wchar_t *fallback) {
    for (int i = 2; i + 1 < argc; ++i)
        if (!wcscmp(argv[i], name)) return argv[i + 1];
    return fallback ? fallback : L"";
}

static bool load_manifest(Engine &engine, const std::wstring &path) {
    std::string text, error;
    if (!read_file(path, text) || !engine.manifest.parse(text, error)) {
        fprintf(stderr, "cannot read manifest %s %s\n", narrow(path).c_str(), error.c_str());
        return false;
    }
    return true;
}

static void summary(const Engine &engine, const Result &result) {
    printf("lines %lld (invalid %lld); counters %zu; unlocked %zu:", result.lines, result.bad_lines,
           engine.state.counters.size(), engine.state.unlocked.size());
    for (const auto &entry : engine.state.unlocked) printf(" %s", entry.first.c_str());
    printf("\n");
    for (const auto &id : engine.manifest.achievements) {
        double current, target;
        if (!engine.state.unlocked.count(id) && engine.progress(id, current, target))
            printf("  progress %s %g/%g\n", id.c_str(), current, target);
    }
}

static int replay(int argc, wchar_t **argv) {
    Engine engine;
    engine.game = narrow(argument(argc, argv, L"--game", L"fh2"));
    engine.clock = [] { return std::string("replay"); };
    if (!load_manifest(engine, argument(argc, argv, L"--manifest", nullptr))) return 2;
    Result result;
    for (int i = 2; i < argc; ++i) {
        if (argv[i][0] == L'-' && argv[i][1] == L'-') { ++i; continue; }
        long long end = replay_file(engine, argv[i], 0, result);
        printf("replayed %s up to byte %lld\n", narrow(argv[i]).c_str(), end);
    }
    summary(engine, result);
    std::wstring out = argument(argc, argv, L"--out", nullptr);
    return write_file_atomic(out, engine.state.to_json(engine.game)) ? 0 : 3;
}

static int migrate_command(int argc, wchar_t **argv) {
    Engine engine;
    std::wstring game = argument(argc, argv, L"--game", L"fh2");
    engine.game = narrow(game);
    engine.clock = [] { return std::string("migrated-replay"); };
    if (!load_manifest(engine, argument(argc, argv, L"--manifest", nullptr))) return 2;
    StorePaths paths;
    paths.set(argument(argc, argv, L"--data", nullptr), game);
    Result result;
    std::string report;
    migrate(engine, paths, result, report);
    printf("%s\n", report.c_str());
    summary(engine, result);
    std::wstring out = argument(argc, argv, L"--out", nullptr);
    return write_file_atomic(out, engine.state.to_json(engine.game)) ? 0 : 3;
}

static int compare(int argc, wchar_t **argv) {
    if (argc < 4) return 2;
    State expected, actual;
    std::string a, b;
    if (!read_file(argv[2], a) || !expected.parse(a, true, nullptr, nullptr) ||
        !read_file(argv[3], b) || !actual.parse(b, true, nullptr, nullptr)) {
        fprintf(stderr, "cannot read the states\n");
        return 2;
    }
    int differences = 0;
    std::vector<std::string> keys;
    for (const auto &e : expected.counters) keys.push_back(e.first);
    for (const auto &e : actual.counters)
        if (!expected.counters.count(e.first)) keys.push_back(e.first);
    size_t equal = 0;
    for (const auto &key : keys) {
        auto x = expected.counters.find(key), y = actual.counters.find(key);
        bool hx = x != expected.counters.end(), hy = y != actual.counters.end();
        if (hx && hy && std::fabs(x->second - y->second) < 1e-9) { ++equal; continue; }
        ++differences;
        printf("  counter %-48s expected %-12s actual %s\n", key.c_str(),
               hx ? std::to_string((long long)x->second).c_str() : "(none)",
               hy ? std::to_string((long long)y->second).c_str() : "(none)");
    }
    size_t unlocks_equal = 0;
    for (const auto &e : expected.unlocked) {
        if (actual.unlocked.count(e.first)) { ++unlocks_equal; continue; }
        ++differences;
        printf("  unlock  %-6s expected, missing\n", e.first.c_str());
    }
    for (const auto &e : actual.unlocked)
        if (!expected.unlocked.count(e.first)) {
            ++differences;
            printf("  unlock  %-6s extra\n", e.first.c_str());
        }
    printf("counters equal %zu/%zu, unlocks equal %zu (expected %zu, actual %zu): %s\n", equal, keys.size(),
           unlocks_equal, expected.unlocked.size(), actual.unlocked.size(), differences ? "DIFFERENT" : "IDENTICAL");
    return differences ? 1 : 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc >= 2 && !wcscmp(argv[1], L"replay")) return replay(argc, argv);
    if (argc >= 2 && !wcscmp(argv[1], L"migrate")) return migrate_command(argc, argv);
    if (argc >= 2 && !wcscmp(argv[1], L"compare")) return compare(argc, argv);
    fprintf(stderr, "usage: ach-replay replay|migrate|compare ...\n");
    return 2;
}
