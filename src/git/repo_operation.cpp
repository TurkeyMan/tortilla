#include "repo_operation.h"

#include "git/command.h"

#include <rapport/sys.h>

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{
namespace
{

std::string read_file(const fs::path& p)
{
    std::string out;
    if (std::FILE* f = rap::sys::fopen(p, "rb"))
    {
        char buf[1024];
        while (size_t n = std::fread(buf, 1, sizeof buf, f)) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

// "key=value" from the marker's lines.
std::string field(std::string_view text, std::string_view key)
{
    for (size_t at = 0; at < text.size();)
    {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(at, nl - at);
        at = nl + 1;
        if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == '=')
            return std::string(line.substr(key.size() + 1));
    }
    return {};
}

}  // namespace

operation_state operation_in_git_dir(const std::string& git_dir)
{
    operation_state s;
    if (git_dir.empty()) return s;
    const fs::path dir = rap::sys::path_from_utf8(git_dir);
    std::error_code ec;
    auto there = [&](const char* name) { return fs::exists(dir / name, ec); };
    // Tortilla's first: its picks leave CHERRY_PICK_HEAD too. It works on a
    // detached HEAD, so a marker left behind (a crash) while a branch is
    // checked out doesn't count.
    if (there("tortilla-rebase-active") && !read_file(dir / "HEAD").starts_with("ref:"))
    {
        s.operation = repo_operation::tortilla_rebase;
        const std::string text = read_file(dir / "tortilla-rebase-active");
        s.onto = field(text, "onto");
        s.branch = field(text, "branch");
    }
    else if (there("rebase-merge"))
        s.operation = repo_operation::rebase;
    else if (there("rebase-apply"))
        s.operation = fs::exists(dir / "rebase-apply" / "applying", ec)
                          ? repo_operation::apply_mailbox
                          : repo_operation::rebase;
    else if (there("MERGE_HEAD"))
        s.operation = repo_operation::merge;
    else if (there("CHERRY_PICK_HEAD"))
        s.operation = repo_operation::cherry_pick;
    else if (there("REVERT_HEAD"))
        s.operation = repo_operation::revert;
    return s;
}

operation_state operation_in_progress(const std::string& workdir)
{
    std::string dir =
        run_git(workdir, {"rev-parse", "--absolute-git-dir"}, git_output::stdout_only).output;
    while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r')) dir.pop_back();
    return operation_in_git_dir(dir);
}

}  // namespace tortilla
