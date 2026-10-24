// The log window's changed-files list (bottom pane) [spec 1.11.2].
#pragma once

#include "git/repo.h"
#include "i18n.h"

#include <rapport/rapport.h>

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace tortilla
{

class file_model : public rap::list_model
{
public:
    enum column : size_t
    {
        path,
        extension,
        status,
        added,
        removed,
    };

    static std::vector<rap::column> columns()
    {
        using namespace rap::literals;
        return {{.title = _("Path"), .stretch = true},
                {.title = _("Extension"), .width = 10_ch},
                {.title = _("Status"), .width = 12_ch},
                {.title = _("Added"), .width = 8_ch, .align = rap::align::end},
                {.title = _("Removed"), .width = 9_ch, .align = rap::align::end}};
    }

    void set(std::vector<file_change> files, bool show_unversioned = true)
    {
        files_.clear();
        checked_.clear();
        for (auto& f : files)
            if (show_unversioned || f.status != '?')
            {
                // A box the user set keeps its state across reloads; new
                // rows start as TortoiseGit's Commit does: changes to
                // versioned files checked, the rest not [spec 5.4.5].
                auto it = remembered_.find(f.path);
                checked_.push_back(it != remembered_.end() ? it->second : starts_checked(f.status));
                files_.push_back(std::move(f));
            }
        notify_reset();
    }

    static bool starts_checked(char status)
    {
        return status != '?' && status != '!' && status != 'S' && status != 'h';
    }

    // Row checkboxes (lists made with them): what toggle_checked does is
    // the owner's (TortoiseGit applies it to the selection).
    std::function<void(size_t row)> on_toggle;
    rap::check_state checked(size_t row) const override
    {
        return row < checked_.size() && checked_[row] ? rap::check_state::checked
                                                      : rap::check_state::unchecked;
    }
    void toggle_checked(size_t row) override
    {
        if (on_toggle) on_toggle(row);
    }
    bool is_checked(size_t row) const { return row < checked_.size() && checked_[row]; }
    void set_checked(const std::vector<size_t>& rows, bool on)
    {
        for (size_t r : rows)
            if (r < checked_.size())
            {
                checked_[r] = on;
                remembered_[files_[r].path] = on;
                notify_changed(r, 1);
            }
    }
    size_t checked_count() const { return size_t(std::ranges::count(checked_, true)); }
    // Forget the user's choices (a new set of files).
    void forget_checks() { remembered_.clear(); }

    // Line counts arriving after the list: files[i] is the (first + i)th
    // file given to set(). Rows keep their place and selection.
    void set_counts(size_t first, const std::vector<file_change>& files)
    {
        size_t lo = files_.size(), hi = 0;
        for (size_t i = 0; i < files.size(); ++i)
        {
            const size_t row = first + i;
            if (row >= files_.size() || files_[row].path != files[i].path) continue;
            files_[row] = files[i];
            lo = std::min(lo, row);
            hi = row + 1;
        }
        if (lo < hi) notify_changed(lo, hi - lo);
    }

    const std::vector<file_change>& files() const { return files_; }

    size_t row_count() const override { return files_.size(); }

    // The file's type, as the file manager shows it, before its path.
    rap::icon icon(size_t row, size_t column) const override
    {
        if (column != path || row >= files_.size()) return {};
        return rap::icon::for_file(files_[row].path);
    }

    std::string text(size_t row, size_t column) const override
    {
        const auto& f = files_[row];
        switch (column)
        {
        case path:
            return f.old_path.empty() ? f.path
                                      : rap::i18n::format(_("{0} (from {1})"), f.path, f.old_path);
        case extension:
        {
            auto slash = f.path.rfind('/');
            auto dot = f.path.rfind('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
            return f.path.substr(dot);
        }
        case status: return status_name(f.status);
        case added: return f.binary || f.status == '?' || !f.counted ? "" : std::to_string(f.added);
        case removed:
            return f.binary || f.status == '?' || !f.counted ? "" : std::to_string(f.removed);
        }
        return {};
    }

    // TortoiseGit's status colours [spec 1.11.2, GitStatusListCtrl].
    rap::text_style style(size_t row, size_t) const override
    {
        if (row >= files_.size()) return {};
        auto rgb = [](uint8_t r, uint8_t g, uint8_t b)
        { return rap::text_style{.fg = rap::color{r, g, b}}; };
        switch (files_[row].status)
        {
        case 'U': return rgb(255, 0, 0);
        case 'M':
        case 'T': return rgb(0, 50, 160);
        case 'A':
        case 'C': return rgb(100, 0, 100);
        case 'D': return rgb(100, 0, 0);
        case 'R': return rgb(0, 0, 255);
        }
        return {};
    }

    static std::string status_name(char s)
    {
        switch (s)
        {
        case 'A': return C_("status", "Added");
        case 'D': return C_("status", "Deleted");
        case 'M': return C_("status", "Modified");
        case 'R': return C_("status", "Renamed");
        case 'C': return C_("status", "Copied");
        case 'T': return C_("status", "Type change");
        case '?': return C_("status", "Unversioned");
        case 'U': return C_("status", "Conflicted");
        case '!': return C_("status", "Ignored");
        case 'S': return C_("status", "Skip worktree");
        case 'h': return C_("status", "Assume unchanged");
        }
        return std::string(1, s);
    }

private:
    std::vector<file_change> files_;
    std::vector<bool> checked_;                         // per row, with checkboxes
    std::unordered_map<std::string, bool> remembered_;  // by path, as the user set them
};

}  // namespace tortilla
