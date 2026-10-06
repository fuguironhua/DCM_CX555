#pragma once
#include <filesystem>
#include <string>

namespace CeTableUpdater {
struct Result {
    bool success = false;
    std::wstring message;
};
// Uses the existing CE named-pipe bridge. Does not launch/attach CE or the game.
Result Update(const std::filesystem::path& configDirectory,
              const std::filesystem::path& offsetsFile, bool dryRun = false);
}
