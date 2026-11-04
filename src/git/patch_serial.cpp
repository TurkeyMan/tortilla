#include "patch_serial.h"

#include "git/diff_tool.h"
#include "git/ignore_patterns.h"
#include "git/remotes.h"
#include "util/entry_path.h"

#include <git2.h>
#include <rapport/sys.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <memory>

namespace tortilla
{

std::vector<std::string> format_patch_serial_args(const format_patch_options& o)
{
    std::vector<std::string> args{"format-patch", "-o", o.folder};
    switch (o.commits)
    {
    case format_patch_options::which::since: args.push_back(o.since); break;
    case format_patch_options::which::number:
        args.push_back("-" + std::to_string(o.number < 1 ? 1 : o.number));
        break;
    case format_patch_options::which::range:
        args.push_back(o.from + ".." + (o.to.empty() ? std::string("HEAD") : o.to));
        break;
    }
    return args;
}

std::vector<std::string> am_args(const am_options& o, const std::vector<std::string>& patches)
{
    std::vector<std::string> args{"am"};
    if (o.three_way) args.push_back("--3way");
    if (o.ignore_space) args.push_back("--ignore-space-change");
    if (o.keep_cr) args.push_back("--keep-cr");
    if (o.signoff) args.push_back("--signoff");
    args.push_back("--");
    args.insert(args.end(), patches.begin(), patches.end());
    return args;
}

bool am_in_progress(const std::string& git_dir)
{
    std::error_code ec;
    return std::filesystem::exists(rap::sys::path_from_utf8(git_dir) / "rebase-apply" / "applying",
                                   ec);
}

namespace
{

// The patch as libgit2 reads it: each file's old and new name and change,
// which git apply's --numstat leaves out (a rename shows its new name
// only). Nothing if it can't read it (a plain diff without git's headers).
std::vector<patch_entry> parsed_entries(std::string_view patch)
{
    static const int init = git_libgit2_init();
    (void)init;
    std::vector<patch_entry> out;
    git_diff* raw = nullptr;
    if (patch.empty() || git_diff_from_buffer(&raw, patch.data(), patch.size()) != 0) return out;
    std::unique_ptr<git_diff, decltype(&git_diff_free)> diff(raw, git_diff_free);
    for (size_t i = 0, n = git_diff_num_deltas(diff.get()); i < n; ++i)
    {
        const git_diff_delta* d = git_diff_get_delta(diff.get(), i);
        patch_entry e;
        e.old_path = d->old_file.path ? d->old_file.path : "";
        e.path = d->new_file.path ? d->new_file.path : e.old_path;
        if (e.old_path.empty()) e.old_path = e.path;
        switch (d->status)
        {
        case GIT_DELTA_ADDED: e.kind = patch_entry::change::added; break;
        case GIT_DELTA_DELETED: e.kind = patch_entry::change::deleted; break;
        case GIT_DELTA_RENAMED: e.kind = patch_entry::change::renamed; break;
        case GIT_DELTA_COPIED: e.kind = patch_entry::change::copied; break;
        default: e.kind = patch_entry::change::modified; break;
        }
        e.binary = (d->flags & GIT_DIFF_FLAG_BINARY) != 0;
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace

std::vector<patch_entry> patch_entries(std::string_view numstat, std::string_view patch)
{
    // "added\tdeleted\tpath\0" ("-\t-\t" for a binary file), or as git diff
    // writes a rename, "added\tdeleted\t\0old\0new\0".
    std::vector<patch_entry> out;
    std::vector<patch_entry> parsed = parsed_entries(patch);
    auto field = [&](std::string_view& s)
    {
        const size_t end = s.find('\0');
        std::string_view f = s.substr(0, end);
        s = end == std::string_view::npos ? std::string_view() : s.substr(end + 1);
        return f;
    };
    while (!numstat.empty())
    {
        std::string_view record = field(numstat);
        const size_t tab = record.find('\t');
        const size_t tab2 = tab == std::string_view::npos ? tab : record.find('\t', tab + 1);
        if (tab2 == std::string_view::npos) continue;
        std::string_view path = record.substr(tab2 + 1), old;
        if (path.empty())
        {
            old = field(numstat);
            path = field(numstat);
        }
        if (path.empty()) continue;
        patch_entry e{.old_path = std::string(old.empty() ? path : old), .path = std::string(path)};
        e.binary = record.starts_with("-\t-\t");
        if (!old.empty() && old != path) e.kind = patch_entry::change::renamed;
        // git apply's list, git apply's names (--include matches the new
        // one); the rest from the patch read through, in the same order.
        if (auto it =
                std::ranges::find_if(parsed, [&](const patch_entry& p) { return p.path == path; });
            it != parsed.end())
        {
            e.old_path = it->old_path;
            e.kind = it->kind;
            e.binary = e.binary || it->binary;
            parsed.erase(it);
        }
        out.push_back(std::move(e));
    }
    return out;
}

patch_preview preview_patch_entry(const std::string& workdir, const std::string& patch,
                                  const patch_entry& e, const std::filesystem::path& folder)
{
    namespace fs = std::filesystem;
    patch_preview out;
    // The patch's names are anyone's: each must land in the folder as itself.
    const auto old_rel = entry_path(e.old_path);
    const auto new_rel = entry_path(e.path);
    if (!old_rel || !new_rel)
    {
        out.applied.output = e.path + ": not a name this system can hold";
        return out;
    }
    const fs::path original = rap::sys::path_from_utf8(workdir) / *old_rel;
    const fs::path base = folder / *old_rel, patched = folder / *new_rel;
    std::error_code ec;
    // What the patch starts from: the old file (a rename's, a copy's
    // source), as it is in the working tree; nothing for an added one.
    const bool existed = e.kind != patch_entry::change::added &&
                         fs::is_regular_file(fs::symlink_status(original, ec));
    if (existed)
    {
        fs::create_directories(base.parent_path(), ec);
        fs::copy_file(original, base, ec);
    }
    // git apply outside a repository works as patch does, but with the
    // repository's line endings: Windows' global autocrlf would otherwise
    // write a new file CRLF where the repository writes it as it is.
    std::vector<std::string> args;
    for (const char* key : {"core.autocrlf", "core.eol"})
        if (const std::string v = config_value(workdir, key); !v.empty())
            args.insert(args.end(), {"-c", std::string(key) + "=" + v});
    std::ranges::move(apply_args(patch, {e.path}), std::back_inserter(args));
    out.applied = run_git(rap::sys::path_to_utf8(folder), args);
    out.base = existed ? rap::sys::path_to_utf8(original) : empty_file();
    out.mine = fs::is_regular_file(fs::symlink_status(patched, ec))
                   ? rap::sys::path_to_utf8(patched)
                   : empty_file();
    return out;
}

std::vector<std::string> apply_args(const std::string& patch, const std::vector<std::string>& only,
                                    bool check)
{
    std::vector<std::string> args{"apply"};
    if (check) args.push_back("--check");
    // --include takes a pattern (git's wildmatch, as .gitignore's): each
    // name escaped, so [ab].txt is that file and never a.txt and b.txt.
    for (const std::string& p : only) args.push_back("--include=" + literal_pattern(p));
    args.insert(args.end(), {"--", patch});
    return args;
}

}  // namespace tortilla
