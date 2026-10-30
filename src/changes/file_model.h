// The log window's changed-files list (bottom pane) [spec 1.11.2].
#pragma once

#include "git/repo.h"
#include "i18n.h"

#include <rapport/rapport.h>

#include <algorithm>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace tortilla
{

// Whether `path` is the repository-relative `related` path or inside it; ""
// holds everything. The Log dialog's "unrelated" files are those outside the
// path it was opened for [spec 1.9.5].
inline bool path_is_related(std::string_view path, std::string_view related)
{
    while (!related.empty() && related.back() == '/') related.remove_suffix(1);
    if (related.empty()) return true;
    return path.starts_with(related) &&
           (path.size() == related.size() || path[related.size()] == '/');
}

// TortoiseGit's "Filter paths" box: a substring of the path, in any case
// [spec 1.11.2]. ASCII case only: git compares paths bytewise.
inline bool path_matches_filter(std::string_view path, std::string_view filter)
{
    auto lower = [](char c) { return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    return filter.empty() || !std::ranges::search(path, filter, {}, lower, lower).empty();
}

// Which of a list's files show, and which are greyed: the Log dialog's
// View > Hide/Gray Unrelated Changed Paths and its path filter box.
struct file_view
{
    enum unrelated_mode
    {
        show_unrelated,
        gray_unrelated,  // TortoiseGit's default
        hide_unrelated,
    };
    std::string related;  // "" when the log isn't limited to a path
    unrelated_mode unrelated = gray_unrelated;
    std::string filter;  // "" for every file

    // A rename or copy counts by either of its paths.
    bool related_to(const file_change& f) const
    {
        return path_is_related(f.path, related) ||
               (!f.old_path.empty() && path_is_related(f.old_path, related));
    }
    bool shows(const file_change& f) const
    {
        if (unrelated == hide_unrelated && !related_to(f)) return false;
        return path_matches_filter(f.path, filter) ||
               (!f.old_path.empty() && path_matches_filter(f.old_path, filter));
    }
    bool grays(const file_change& f) const { return unrelated == gray_unrelated && !related_to(f); }
};

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
        all_.clear();
        for (auto& f : files)
            if (show_unversioned || f.status != '?') all_.push_back(std::move(f));
        refilter();
    }

    // Shows (or greys) the files `view` picks from those set; the rows are
    // rebuilt, as for a new set of files.
    void set_view(file_view view)
    {
        view_ = std::move(view);
        refilter();
    }
    // Files set, hidden ones included.
    size_t total() const { return all_.size(); }

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
            const size_t at = first + i;
            if (at >= all_.size() || all_[at].path != files[i].path) continue;
            all_[at] = files[i];
            const size_t row = row_of_[at];
            if (row >= files_.size()) continue;  // hidden by the view
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
        // Grey comes before the status colours [spec 1.11.2].
        if (grayed_[row]) return {.fg = rap::theme_color::placeholder_text};
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
    void refilter()
    {
        files_.clear();
        checked_.clear();
        grayed_.clear();
        row_of_.assign(all_.size(), size_t(-1));
        for (size_t i = 0; i < all_.size(); ++i)
        {
            const file_change& f = all_[i];
            if (!view_.shows(f)) continue;
            // A box the user set keeps its state across reloads; new
            // rows start as TortoiseGit's Commit does: changes to
            // versioned files checked, the rest not [spec 5.4.5].
            auto it = remembered_.find(f.path);
            checked_.push_back(it != remembered_.end() ? it->second : starts_checked(f.status));
            grayed_.push_back(view_.grays(f));
            row_of_[i] = files_.size();
            files_.push_back(f);
        }
        notify_reset();
    }

    std::vector<file_change> all_;  // as set, less unversioned ones if asked
    std::vector<size_t> row_of_;    // per all_: its row, or size_t(-1) when hidden
    // Only the log sets one; other lists show everything.
    file_view view_{.unrelated = file_view::show_unrelated};
    std::vector<file_change> files_;                    // the rows
    std::vector<bool> grayed_;                          // per row
    std::vector<bool> checked_;                         // per row, with checkboxes
    std::unordered_map<std::string, bool> remembered_;  // by path, as the user set them
};

}  // namespace tortilla
