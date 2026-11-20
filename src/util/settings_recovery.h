// Durable, bounded settings-replacement receipts, shared with shell readers.
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace tortilla
{
struct settings_recovery_snapshot
{
    uint64_t volume = 0, generation = 0, length = 0;
    std::array<unsigned char, 16> id{};
    std::string hash;
    bool generation_present = false;
};
struct settings_recovery_record
{
    std::filesystem::path target, alias, backup, replacement;
    settings_recovery_snapshot old_file, new_file, reservation;
    std::string transaction;
    std::vector<std::filesystem::path> receipts;
};
enum class settings_recovery_phase { clear, prepared, recoverable, committed, blocked };
struct settings_recovery_view
{
    std::filesystem::path target, readable;
    settings_recovery_phase phase = settings_recovery_phase::clear;
    std::string message;
    std::vector<settings_recovery_record> records;
};
struct settings_recovery_step
{
    bool restored = false;
    std::string error;
};
// Read-only: never restores or deletes anything. A missing target may read a
// complete identity/hash-verified last-good backup. Invalid/ambiguous receipts
// block writes; PID and modification times never select a winner.
settings_recovery_view inspect_settings_recovery(const std::filesystem::path&);
settings_recovery_step recover_settings_before_save(const std::filesystem::path&,
                                                    std::chrono::steady_clock::time_point deadline);
std::expected<settings_recovery_record, std::string> begin_settings_recovery(
    const std::filesystem::path& target, const std::filesystem::path& configured,
    const std::filesystem::path& backup, const std::filesystem::path& replacement);
// Delete receipts only AFTER the owned backup has been disposed/restored.
std::string finish_settings_recovery(const settings_recovery_record&);
void settings_recovery_fail_restore_for_tests(bool);
}  // namespace tortilla
