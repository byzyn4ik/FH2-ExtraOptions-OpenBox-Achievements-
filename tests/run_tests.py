"""Offline tests of the OpenBox Achievements plugin (no game, no real OpenBox).

Usage: py tests/run_tests.py [--output out] [--openbox <OpenBox app repo or folder>]

1. package: mod.json as Extra Options 0.3.2 validates it, the plugin layout,
   the DLL (PE32+ x64, the loader's exports, only system imports), the INI.
2. rules: the built-in rules' achievement IDs match OpenBox's FH2 manifest
   (when the OpenBox app repo is found) and that manifest has no rules.
3. replay: tests/data/*.jsonl through out/tools/ach-replay.exe, checked
   against the matching *.expected.json (unlocks, progress, line counts).
4. plugin: out/tests/plugin_test.exe loads the built DLL with a fake FH2
   image, a fake OpenBox pipe server and a fake OpenBox app (modes full, late,
   mismatch, badbutton, noapp), with private folders and pipe names.
"""

import argparse
import configparser
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MOD_NAME = "OpenBox Achievements"
DLL_NAME = "OpenBoxAchievements.dll"
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|advapi32|shell32|ole32|ntdll|msvcrt|api-ms-win-[a-z0-9-]+)\.dll$", re.I)

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures.append(what)


# ------------------------------------------------------------------ PE reader

class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if d[:2] != b"MZ" or d[e:e + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.machine, sections, _, _, _, optional_size, self.characteristics = struct.unpack_from("<HHIIIHH", d, e + 4)
        optional = e + 24
        self.magic = struct.unpack_from("<H", d, optional)[0]
        self.sections = []
        table = optional + optional_size
        for i in range(sections):
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, table + 40 * i)
            self.sections.append((va, max(vsize, rsize), raw))
        directories = optional + 112
        self.exports_rva, _ = struct.unpack_from("<II", d, directories)
        self.imports_rva, _ = struct.unpack_from("<II", d, directories + 8)

    def offset(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(hex(rva))

    def text(self, rva):
        o = self.offset(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("ascii")

    def exports(self):
        if not self.exports_rva:
            return []
        o = self.offset(self.exports_rva)
        count, names = struct.unpack_from("<II", self.data, o + 24)[0], struct.unpack_from("<I", self.data, o + 32)[0]
        return [self.text(struct.unpack_from("<I", self.data, self.offset(names) + 4 * i)[0]) for i in range(count)]

    def imports(self):
        result, o = [], self.offset(self.imports_rva) if self.imports_rva else None
        while o is not None:
            fields = struct.unpack_from("<IIIII", self.data, o)
            if not fields[3]:
                break
            result.append(self.text(fields[3]))
            o += 20
        return result


# --------------------------------------------------------------- 1. package

def valid_mod_id(value):
    # features/addons.c valid_id() in Extra Options 0.3.2.
    if not isinstance(value, str) or not 0 < len(value) <= 128:
        return False
    for i, ch in enumerate(value):
        if not (ch.islower() and ch.isascii() or ch.isdigit() or ch == "-"):
            return False
        if ch == "-" and (i == 0 or i == len(value) - 1 or value[i - 1] == "-"):
            return False
    return True


def test_package(output):
    print("package")
    mod = output / "mods" / MOD_NAME
    manifest = json.loads((mod / "mod.json").read_text(encoding="utf-8"))
    check(manifest.get("format_version") == 1, "mod.json format_version is 1")
    check(valid_mod_id(manifest.get("id")), f"mod.json id {manifest.get('id')!r} is a valid Extra Options mod ID")
    check(isinstance(manifest.get("dependencies", []), list), "mod.json dependencies is a list")
    for key in ("database", "media", "strings"):
        check(manifest.get(key, None) is None or isinstance(manifest[key], str), f"mod.json {key} is absent, null or text")
    check(not (mod / "database.json").exists() and not (mod / "strings.json").exists(),
          "no database or strings file (a plugin-only mod)")
    dll = mod / "plugins" / DLL_NAME
    check(dll.is_file(), "plugins/OpenBoxAchievements.dll is there")
    top = sorted(p.name for p in (mod / "plugins").iterdir())
    check(top == [DLL_NAME, "OpenBoxAchievements.ini"], f"plugins folder holds the DLL and its INI only ({top})")
    pe = PE(dll)
    check(pe.machine == 0x8664 and pe.magic == 0x20B, "the DLL is PE32+ x64")
    check(bool(pe.characteristics & 0x2000), "the image is a DLL")
    exports = pe.exports()
    check("FH2PluginInitialize" in exports and "FH2PluginVehicleUpdate" in exports,
          f"exports FH2PluginInitialize and FH2PluginVehicleUpdate ({exports})")
    check("FH2PluginDatabaseUpdate" not in exports and "FH2PluginMediaDirectory" not in exports,
          "no database or media callbacks exported")
    imports = pe.imports()
    foreign = [name for name in imports if not SYSTEM_DLLS.match(name)]
    check(not foreign, f"only Windows DLLs imported ({', '.join(imports)})")
    ini = configparser.ConfigParser(interpolation=None, strict=True)
    ini.optionxform = str
    ini.read(mod / "plugins" / "OpenBoxAchievements.ini", encoding="utf-8")
    expected = {"Achievements": ["Enabled", "Game", "PauseButton", "Saves", "Rules"],
                "OpenBox": ["AppFolder", "StartWithGame", "Data", "Pipe"], "Logging": ["Enabled", "Telemetry"]}
    for section, keys in expected.items():
        check(ini.has_section(section) and all(ini.has_option(section, k) for k in keys),
              f"INI [{section}] has {', '.join(keys)}")
    check(ini.get("Achievements", "Enabled", fallback="") == "1", "INI [Achievements] Enabled defaults to 1")


# ------------------------------------------------------------------ 2. rules

def test_rules(openbox):
    print("rules")
    rules = json.loads((ROOT / "rules" / "fh2_achievement_rules.json").read_text(encoding="utf-8"))
    ids = rules["Achievements"]
    check(len(ids) == len(set(ids)), "achievement IDs are unique")
    check(all(r["Achievement"] in ids for r in rules["Rules"]), "every rule names a listed achievement")
    candidates = [openbox] if openbox else [ROOT.parent / "OpenBoxAchievements"]
    manifest = None
    for base in candidates:
        for path in (Path(base) / "games" / "fh2" / "achievement_manifest.json",):
            if path.is_file():
                manifest = path
    if not manifest:
        print("  skip OpenBox's FH2 manifest not found (--openbox)")
        return
    data = json.loads(manifest.read_text(encoding="utf-8-sig"))
    check(data.get("Achievements") == ids, f"IDs equal OpenBox's {manifest}")
    check("Rules" not in data, "OpenBox's manifest has no rules (they live in this plugin)")


# ----------------------------------------------------------------- 3. replay

def parse_summary(text):
    unlocked, progress, lines, invalid = [], {}, None, None
    for line in text.splitlines():
        m = re.match(r"lines (\d+) \(invalid (\d+)\); counters \d+; unlocked \d+:(.*)", line)
        if m:
            lines, invalid, unlocked = int(m[1]), int(m[2]), m[3].split()
        m = re.match(r"\s+progress (\S+) (\S+)/(\S+)", line)
        if m:
            progress[m[1]] = (float(m[2]), float(m[3]))
    return unlocked, progress, lines, invalid


def test_replay(output):
    print("replay")
    tool = output / "tools" / "ach-replay.exe"
    for events in sorted((HERE / "data").glob("*.jsonl")):
        expected = json.loads(events.with_name(events.stem + ".expected.json").read_text(encoding="utf-8"))
        state = output / "test-runs" / (events.stem + ".state.json")
        state.parent.mkdir(parents=True, exist_ok=True)
        run = subprocess.run([str(tool), "replay", "--manifest", str(ROOT / "rules" / "fh2_achievement_rules.json"),
                              "--out", str(state), str(events)], capture_output=True, text=True)
        check(run.returncode == 0, f"{events.name}: ach-replay ran")
        unlocked, progress, lines, invalid = parse_summary(run.stdout)
        check(sorted(unlocked, key=int) == sorted(expected["unlocked"], key=int),
              f"{events.name}: unlocked {sorted(unlocked, key=int)}")
        for id, value in expected["progress"].items():
            current, target = (float(x) for x in value.split("/"))
            check(progress.get(id) == (current, target), f"{events.name}: progress {id} {value}")
        check(lines == expected["lines"] and invalid == expected["invalid"],
              f"{events.name}: {lines} lines, {invalid} invalid")
        saved = json.loads(state.read_text(encoding="utf-8"))
        check(sorted(saved["Unlocked"], key=int) == sorted(expected["unlocked"], key=int),
              f"{events.name}: the state file has the unlocks")


# ----------------------------------------------------------------- 4. plugin

def stat_entry(name, kind, value):
    raw = name.encode("ascii")
    head = struct.pack("<I", len(raw)) + raw + b" " + b"\0\0\0\0" + struct.pack("<I", kind)
    if kind == 15:
        return head + struct.pack("<I", len(value)) + b"".join(value)
    if kind in (3, 7):
        return head + struct.pack("<i", value)
    if kind == 4:
        return head + struct.pack("<q", value)
    if kind == 9:
        return head + struct.pack("<f", value)
    if kind == 12:
        text = value.encode("utf-16-le")
        return head + struct.pack("<I", len(value)) + text
    return head + bytes([value])


def write_save(saves):
    entries = [
        stat_entry("CompareStats", 15, [stat_entry("CarsInGarage", 3, 60), stat_entry("DiscountBoardsSmashed", 3, 30),
                                        stat_entry("Speed", 9, 1.5), stat_entry("Name", 12, "Max")]),
        stat_entry("MiscStats", 15, [stat_entry("UpgradesBoardsSmashed", 7, 5)]),
    ]
    body = b"".join(entries)
    data = b"ssmc" + struct.pack("<II", len(body) + 12, len(entries)) + body
    old = saves / "player" / "User_1" / "revisions" / "1"
    new = saves / "player" / "User_1" / "revisions" / "2"
    for revision, content in ((old, b"ssmc" + struct.pack("<II", 12, 0)), (new, data)):
        (revision / "data").mkdir(parents=True, exist_ok=True)
        (revision / "data" / "ProfileData").write_bytes(content)
        (revision / "commit").write_bytes(b"")
    os.utime(old / "commit", (1_600_000_000, 1_600_000_000))  # older: revision 2 is read


def test_plugin(output):
    print("plugin")
    exe = output / "tests" / "plugin_test.exe"
    for mode in ("full", "late", "mismatch", "badbutton", "noapp"):
        work = output / "test-runs" / mode
        shutil.rmtree(work, ignore_errors=True)
        plugins = work / "mods" / MOD_NAME / "plugins"
        plugins.mkdir(parents=True)
        shutil.copy2(output / "mods" / MOD_NAME / "plugins" / DLL_NAME, plugins / DLL_NAME)
        app = work / "app"
        app.mkdir()
        if mode != "noapp":
            shutil.copy2(exe, app / "OpenBox Achievements.exe")
        saves = work / "saves"
        write_save(saves)
        pipe = f"OpenBoxAchievements.PluginTest.{os.getpid()}.{mode}"
        (plugins / "OpenBoxAchievements.ini").write_text(
            "[Achievements]\nEnabled=1\nGame=fh2\nPauseButton=1\n"
            f"Saves={saves}\nRules=\n"
            f"[OpenBox]\nAppFolder={app}\nStartWithGame={1 if mode == 'full' else 0}\n"
            f"Data={work / 'data'}\nPipe={pipe}\n"
            "[Logging]\nEnabled=1\nTelemetry=1\n", encoding="utf-8")
        try:
            run = subprocess.run([str(exe), mode, str(plugins / DLL_NAME), pipe], capture_output=True, text=True,
                                 timeout=90)
            print(run.stdout.rstrip())
            if run.stderr:
                print(run.stderr.rstrip())
            check(run.returncode == 0, f"plugin_test {mode}")
        except subprocess.TimeoutExpired:
            check(False, f"plugin_test {mode} finished in time")
        if mode == "full":
            log = (plugins / "OpenBoxAchievements.log").read_text(encoding="utf-8", errors="replace")
            check("UNLOCKED achievement 27" in log and "save stats handed to the rules" in log,
                  "full: the plugin's log records the unlocks and the save stats")
            telemetry = (plugins / "OpenBoxAchievements.telemetry.jsonl").read_text(encoding="utf-8")
            check('"e":"CarPurchased"' in telemetry and '"v":"CompareStats/CarsInGarage"' in telemetry,
                  "full: [Logging] Telemetry=1 keeps the records for replays")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--openbox", type=Path, help="the OpenBox Achievements app repo (for the ID check)")
    options = parser.parse_args()
    output = options.output.resolve()
    test_package(output)
    test_rules(options.openbox)
    test_replay(output)
    test_plugin(output)
    print(f"{len(failures)} failure(s)" if failures else "all tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
