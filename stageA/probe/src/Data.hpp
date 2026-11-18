#pragma once
#include <filesystem>
#include <string>

// R1 (0.17.0): where Deckrunner's data lives. A release ships code only. The one-time setup builds this folder on the
// user's machine from their own Skate 3 disc and their own Cyberpunk install (investigation/BACKLOG.md, R1 and R2):
//   <data>\skate\assets   converted Skate 3 data (SK8V's convert-skate-data.py output: private\game.json, skater.glb, ...)
//   <data>\skate\audio    the prepared Skate audio cache (SK8V's audio stages)
//   <data>\rigs\player_woman_skeleton.rig.json, player_man_skeleton.rig.json   V's rigs (WolvenKit JSON of the game's)
// <data> is <plugin dir>\data unless deckrunner.ini says otherwise: [data] dir=, or one part at a time with [data]
// skate_assets=, skate_audio=, rigs= (a folder holding both rig files).
namespace data
{
std::filesystem::path PluginDir();
std::filesystem::path Ini();  // <plugin dir>\deckrunner.ini (optional)
std::filesystem::path Root();
std::filesystem::path SkateAssets();
std::filesystem::path SkateAudio();
std::filesystem::path Rig(bool female);
std::string Utf8(const std::filesystem::path& p);

// What the skater can't start without (Skate's data, and `rig`), as one line naming each missing file; empty when all
// is there. Skate's audio is not required: without it the skater rides silently.
std::string Missing(const std::string& rigUtf8);
} // namespace data
