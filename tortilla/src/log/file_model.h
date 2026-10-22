// The log window's changed-files list (bottom pane) [spec 1.11.2].
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

#include <algorithm>

namespace tortilla
{

class FileModel : public rap::ListModel
{
public:
    enum Column : size_t
    {
        Path,
        Extension,
        Status,
        Added,
        Removed,
    };

    static std::vector<rap::Column> columns()
    {
        using namespace rap::literals;
        return {{.title = "Path", .stretch = true},
                {.title = "Extension", .width = 10_ch},
                {.title = "Status", .width = 12_ch},
                {.title = "Added", .width = 8_ch, .align = rap::Align::End},
                {.title = "Removed", .width = 9_ch, .align = rap::Align::End}};
    }

    void set(std::vector<FileChange> files, bool showUnversioned = true)
    {
        files_.clear();
        for (auto& f : files)
            if (showUnversioned || f.status != '?') files_.push_back(std::move(f));
        notifyReset();
    }

    // Line counts arriving after the list: files[i] is the (first + i)th
    // file given to set(). Rows keep their place and selection.
    void setCounts(size_t first, const std::vector<FileChange>& files)
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
        if (lo < hi) notifyChanged(lo, hi - lo);
    }

    const std::vector<FileChange>& files() const { return files_; }

    size_t rowCount() const override { return files_.size(); }

    // The file's type, as the file manager shows it, before its path.
    rap::Icon icon(size_t row, size_t column) const override
    {
        if (column != Path || row >= files_.size()) return {};
        return rap::Icon::forFile(files_[row].path);
    }

    std::string text(size_t row, size_t column) const override
    {
        const auto& f = files_[row];
        switch (column)
        {
        case Path: return f.oldPath.empty() ? f.path : f.path + " (from " + f.oldPath + ")";
        case Extension:
        {
            auto slash = f.path.rfind('/');
            auto dot = f.path.rfind('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
            return f.path.substr(dot);
        }
        case Status: return statusName(f.status);
        case Added: return f.binary || f.status == '?' || !f.counted ? "" : std::to_string(f.added);
        case Removed: return f.binary || f.status == '?' || !f.counted ? "" : std::to_string(f.removed);
        }
        return {};
    }

    // TortoiseGit's status colours [spec 1.11.2, GitStatusListCtrl].
    rap::TextStyle style(size_t row, size_t) const override
    {
        if (row >= files_.size()) return {};
        auto rgb = [](uint8_t r, uint8_t g, uint8_t b) { return rap::TextStyle{.fg = rap::Color{r, g, b}}; };
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

    static std::string statusName(char s)
    {
        switch (s)
        {
        case 'A': return "Added";
        case 'D': return "Deleted";
        case 'M': return "Modified";
        case 'R': return "Renamed";
        case 'C': return "Copied";
        case 'T': return "Type change";
        case '?': return "Unversioned";
        case 'U': return "Conflicted";
        }
        return std::string(1, s);
    }

private:
    std::vector<FileChange> files_;
};

}  // namespace tortilla
