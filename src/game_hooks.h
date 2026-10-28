// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin's two game hooks, each guarded byte for byte against the
// inspected FH2 release (forza_durango_release_final.exe, SHA-256
// 3ec04c6163fb5614e3d351ba0570d39b846041a257715d6340eceb03ccb7f0a3, the
// executable Extra Options 0.3.2 supports). On any mismatch the hook is not
// installed and the game is left as it is. Neither site is patched by Extra
// Options 0.3.2 itself (checked against its sources and patches.json).
//
// 1. ETX telemetry: the game's import slot for EtwPlus.dll!EtxEventWrite
//    (IAT, RVA 0x1dd0060) is pointed at a wrapper that hands every event to
//    the rules and then calls the original, unchanged.
// 2. The pause menu's own Achievements button (Progress tab,
//    PART_Button_Achievements) opens PopupShowAchievements, whose vtable
//    (0x2e35c40) slot 145 at 0x2e360c8 holds 0xe38770: the method that asks
//    the console's system UI for the title's achievements. The slot is pointed
//    at a handler that brings up OpenBox Achievements, then calls the original.
#pragma once
#include <cstdint>

#include "telemetry.h"

namespace fh2 {
namespace hooks {

constexpr uint32_t kImageSize = 0x3a18000u;
constexpr uint32_t kEtxEventWriteSlot = 0x1dd0060u;
constexpr uint32_t kShowAchievementsSlot = 0x2e360c8u;
constexpr uint32_t kShowAchievementsMethod = 0xe38770u;

// Guards: nullptr when the image is as expected, else what differs.
const char *check_event_table(const unsigned char *image, uint32_t size);
const char *check_etx_import(const unsigned char *image, uint32_t size);
const char *check_achievements_button(const unsigned char *image, uint32_t size);

// ---- ETX telemetry wrapper

typedef void (*EventSink)(const void *event, uint32_t count, const EventData *data);

// Arms the wrapper (after both ETX guards passed); events go to `sink`.
void etx_prepare(unsigned char *image, EventSink sink);
enum EnsureResult {
    EtxAlreadyInstalled,  // nothing to do
    EtxInstalled,         // the wrapper was (re)installed now
    EtxNotReady,          // the slot holds no resolved import yet
    EtxNotCode,           // the slot holds something that is not code: left alone
    EtxChained,           // something else replaced the import after us: left alone
    EtxNotArmed,
};
// Installs the wrapper once the slot holds the resolved import; cheap to repeat.
EnsureResult etx_ensure();
bool etx_installed();

// ---- the pause menu's Achievements button

typedef void (*ShowMethod)(void *page);
// Points the slot at `handler`; `original` gets the game's method. False if
// the guard fails or the slot cannot be written.
bool button_install(unsigned char *image, ShowMethod handler, ShowMethod *original);

} // namespace hooks
} // namespace fh2
