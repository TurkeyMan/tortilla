#include "submodule.h"

#include <rapport/sys.h>

#include <git2.h>

#include <algorithm>
#include <filesystem>

namespace tortilla
{

std::expected<std::vector<submodule_info>, std::string> list_submodules(const repo& r)
{
    std::vector<submodule_info> out;
    auto each = [](git_submodule* sm, const char*, void* payload) -> int
    {
        auto& list = *static_cast<std::vector<submodule_info>*>(payload);
        submodule_info info;
        info.name = git_submodule_name(sm);
        info.path = git_submodule_path(sm);
        if (const char* url = git_submodule_url(sm)) info.url = url;
        // Whether its folder has a .git, without looking further inside it
        // (a status of its files would cost seconds in a big one).
        unsigned int where = 0;
        git_submodule_location(&where, sm);
        info.checked_out = (where & GIT_SUBMODULE_STATUS_IN_WD) != 0;
        list.push_back(std::move(info));
        return 0;
    };
    if (git_submodule_foreach(r.raw(), each, &out) != 0) return std::unexpected(last_git_error());
    // Initialized: its URL copied to the repository's config (libgit2's
    // IN_CONFIG means .gitmodules).
    git_config* config = nullptr;
    if (git_repository_config_snapshot(&config, r.raw()) == 0)
    {
        for (submodule_info& s : out)
        {
            git_buf url = GIT_BUF_INIT;
            s.initialized = git_config_get_string_buf(
                                &url, config, ("submodule." + s.name + ".url").c_str()) == 0;
            git_buf_dispose(&url);
        }
        git_config_free(config);
    }
    std::ranges::sort(out, {}, &submodule_info::path);
    return out;
}

bool has_submodules(const std::string& workdir)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(rap::sys::path_from_utf8(workdir) / ".gitmodules", ec);
}

std::vector<std::string> submodule_add_args(const submodule_add_options& o)
{
    std::vector<std::string> args{"submodule", "add"};
    if (!o.branch.empty()) args.insert(args.end(), {"-b", o.branch});
    if (o.force) args.push_back("-f");
    args.insert(args.end(), {"--", o.url, o.path});
    return args;
}

std::vector<std::string> submodule_update_args(const submodule_update_options& o,
                                               const std::vector<std::string>& paths)
{
    using how = submodule_update_options::how;
    std::vector<std::string> args{"submodule", "update"};
    if (o.init) args.push_back("--init");
    if (o.recursive) args.push_back("--recursive");
    if (o.remote) args.push_back("--remote");
    if (o.no_fetch) args.push_back("--no-fetch");
    if (o.force) args.push_back("--force");
    if (o.mode == how::checkout) args.push_back("--checkout");
    if (o.mode == how::merge) args.push_back("--merge");
    if (o.mode == how::rebase) args.push_back("--rebase");
    args.push_back("--");
    args.insert(args.end(), paths.begin(), paths.end());
    return args;
}

std::vector<std::string> submodule_sync_args(bool recursive)
{
    std::vector<std::string> args{"submodule", "sync"};
    if (recursive) args.push_back("--recursive");
    return args;
}

}  // namespace tortilla
