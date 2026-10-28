// OpenBox Achievements for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Finding and starting the OpenBox Achievements app. Its setup (MSI) records
// the folder in HKCU\Software\OpenBox Achievements\InstallFolder, the value the
// Forza Horizon 2 Builder reads too; [OpenBox] AppFolder in the plugin's INI
// overrides it.
#pragma once
#include <string>

namespace openbox {

constexpr const wchar_t *kAppName = L"OpenBox Achievements.exe";
// The app's single-instance event: it exists while the app runs.
constexpr const wchar_t *kShowEvent = L"Local\\OpenBoxAchievements.Show";

// The app's folder: `configured` when set (full, or relative to `base`),
// else the registry's InstallFolder. Empty when the app is not there.
std::wstring locate(const std::wstring &configured, const std::wstring &base, std::wstring *source);
bool running();
// Starts "<folder>\OpenBox Achievements.exe" <arguments>; a running app takes
// the command line over (it shows the game's page with --game <id>).
bool start(const std::wstring &folder, const std::wstring &arguments);

} // namespace openbox
