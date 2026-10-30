#include "remotes.h"

#include "git/command.h"

#include <git2.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>

namespace tortilla
{
namespace
{

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

std::vector<std::string> lines(const std::string& s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size())
    {
        size_t end = s.find('\n', at);
        if (end == std::string::npos) end = s.size();
        std::string line = trimmed(s.substr(at, end - at));
        if (!line.empty()) out.push_back(std::move(line));
        at = end + 1;
    }
    return out;
}

std::string query(const std::string& workdir, const std::vector<std::string>& args)
{
    const git_result r = run_git(workdir, args, git_output::stdout_only);
    return r.ok() ? trimmed(r.output) : "";
}

// The repository at (or above) `workdir`, through libgit2: these queries
// run on clicks and as dialogs open, where starting git costs most (near
// a second on Windows over \\wsl.localhost). Null if it can't be opened,
// and git answers instead.
using repo_ptr = std::unique_ptr<git_repository, decltype(&git_repository_free)>;
repo_ptr local_repo(const std::string& workdir)
{
    static const int init = git_libgit2_init();
    (void)init;
    git_repository* r = nullptr;
    if (git_repository_open_ext(&r, workdir.empty() ? "." : workdir.c_str(), 0, nullptr) != 0)
        r = nullptr;
    return repo_ptr(r, &git_repository_free);
}

// A revision's commit, as hex; nullopt if libgit2 can't resolve it.
std::optional<std::string> commit_of(git_repository* r, const std::string& rev)
{
    git_object* obj = nullptr;
    if (git_revparse_single(&obj, r, rev.c_str()) != 0) return std::nullopt;
    git_object* commit = nullptr;
    const int peeled = git_object_peel(&commit, obj, GIT_OBJECT_COMMIT);
    git_object_free(obj);
    if (peeled != 0) return std::nullopt;
    char hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(hex, sizeof hex, git_object_id(commit));
    git_object_free(commit);
    return std::string(hex);
}

}  // namespace

std::vector<std::string> remote_names(const std::string& workdir)
{
    if (auto r = local_repo(workdir))
    {
        git_strarray names{};
        if (git_remote_list(&names, r.get()) == 0)
        {
            std::vector<std::string> out(names.strings, names.strings + names.count);
            git_strarray_dispose(&names);
            std::ranges::sort(out);  // as git remote lists them
            return out;
        }
    }
    const git_result r = run_git(workdir, {"remote"}, git_output::stdout_only);
    return r.ok() ? lines(r.output) : std::vector<std::string>{};
}

std::string config_value(const std::string& workdir, const std::string& key)
{
    // libgit2 reads the same files git does (includes and includeIf
    // gitdir/onbranch too); git only when the config can't be read at all.
    if (auto r = local_repo(workdir))
    {
        git_config* config = nullptr;
        if (git_repository_config_snapshot(&config, r.get()) == 0)
        {
            git_buf value{};
            const int found = git_config_get_string_buf(&value, config, key.c_str());
            std::string out = found == 0 && value.ptr ? trimmed(value.ptr) : std::string();
            git_buf_dispose(&value);
            git_config_free(config);
            return out;
        }
    }
    return query(workdir, {"config", "--get", key});
}

std::string current_branch(const std::string& workdir)
{
    // HEAD's own file: a branch's name even before its first commit.
    if (auto r = local_repo(workdir))
    {
        git_reference* head = nullptr;
        std::string out;
        if (git_reference_lookup(&head, r.get(), "HEAD") == 0)
        {
            if (git_reference_type(head) == GIT_REFERENCE_SYMBOLIC)
                if (const char* target = git_reference_symbolic_target(head))
                    out = std::string_view(target).starts_with("refs/heads/") ? target + 11 : "";
            git_reference_free(head);
            return out;
        }
    }
    return query(workdir, {"symbolic-ref", "--short", "-q", "HEAD"});
}

std::vector<std::string> local_branches(const std::string& workdir)
{
    if (auto r = local_repo(workdir))
    {
        git_branch_iterator* it = nullptr;
        if (git_branch_iterator_new(&it, r.get(), GIT_BRANCH_LOCAL) == 0)
        {
            std::vector<std::string> out;
            git_reference* ref = nullptr;
            git_branch_t type;
            while (git_branch_next(&ref, &type, it) == 0)
            {
                const char* name = nullptr;
                if (git_branch_name(&name, ref) == 0 && name) out.emplace_back(name);
                git_reference_free(ref);
            }
            git_branch_iterator_free(it);
            std::ranges::sort(out);  // as for-each-ref lists them
            return out;
        }
    }
    const git_result r =
        run_git(workdir, {"for-each-ref", "--format=%(refname:short)", "refs/heads"},
                git_output::stdout_only);
    return r.ok() ? lines(r.output) : std::vector<std::string>{};
}

std::string resolve(const std::string& workdir, const std::string& rev)
{
    if (auto r = local_repo(workdir))
        if (auto id = commit_of(r.get(), rev)) return *id;
    // Revision syntax libgit2 doesn't know (":/message", say): git's.
    return query(workdir, {"rev-parse", "-q", "--verify", "--end-of-options", rev + "^{commit}"});
}

bool is_ancestor(const std::string& workdir, const std::string& ancestor, const std::string& of)
{
    if (auto r = local_repo(workdir))
    {
        const auto a = commit_of(r.get(), ancestor), b = commit_of(r.get(), of);
        if (a && b)
        {
            git_oid x, y;
            git_oid_fromstr(&x, a->c_str());
            git_oid_fromstr(&y, b->c_str());
            return git_oid_equal(&x, &y) || git_graph_descendant_of(r.get(), &y, &x) == 1;
        }
    }
    return run_git(workdir, {"merge-base", "--is-ancestor", ancestor, of}, git_output::stdout_only)
        .ok();
}

bool is_shallow(const std::string& workdir)
{
    if (auto r = local_repo(workdir)) return git_repository_is_shallow(r.get()) == 1;
    return query(workdir, {"rev-parse", "--is-shallow-repository"}) == "true";
}

bool has_conflicts(const std::string& workdir)
{
    if (auto r = local_repo(workdir))
    {
        git_index* index = nullptr;
        if (git_repository_index(&index, r.get()) == 0)
        {
            const bool conflicts = git_index_has_conflicts(index) == 1;
            git_index_free(index);
            return conflicts;
        }
    }
    const git_result r =
        run_git(workdir, {"diff", "--name-only", "--diff-filter=U"}, git_output::stdout_only);
    return r.ok() && r.output.find_first_not_of(" \r\n") != std::string::npos;
}

bool rebase_in_progress(const std::string& workdir)
{
    auto r = local_repo(workdir);
    if (!r) return false;
    const int state = git_repository_state(r.get());
    return state == GIT_REPOSITORY_STATE_REBASE ||
           state == GIT_REPOSITORY_STATE_REBASE_INTERACTIVE ||
           state == GIT_REPOSITORY_STATE_REBASE_MERGE ||
           state == GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE;
}

upstream upstream_of(const std::string& workdir, const std::string& branch)
{
    if (branch.empty()) return {};
    upstream u{config_value(workdir, "branch." + branch + ".remote"),
               config_value(workdir, "branch." + branch + ".merge")};
    if (u.branch.starts_with("refs/heads/")) u.branch.erase(0, 11);
    return u;
}

std::string push_remote_of(const std::string& workdir, const std::string& branch)
{
    if (!branch.empty())
        if (std::string r = config_value(workdir, "branch." + branch + ".pushremote"); !r.empty())
            return r;
    if (std::string r = config_value(workdir, "remote.pushdefault"); !r.empty()) return r;
    return branch.empty() ? "" : config_value(workdir, "branch." + branch + ".remote");
}

std::string clone_folder_name(std::string url)
{
    std::replace(url.begin(), url.end(), '\\', '/');
    while (!url.empty() && url.back() == '/') url.pop_back();
    size_t cut = url.rfind('/');
    if (cut == std::string::npos) cut = url.rfind(':');
    if (cut == std::string::npos) cut = url.rfind('@');
    std::string name = cut == std::string::npos ? url : url.substr(cut + 1);
    if (name.size() > 4)
    {
        std::string tail = name.substr(name.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (tail == ".git") name.resize(name.size() - 4);
    }
    return name;
}

std::string clone_url_in(std::string_view text)
{
    auto trim = [](std::string_view s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return s;
    };
    std::string_view s = trim(text.substr(0, text.find_first_of("\r\n")));
    if (s.starts_with("git clone "))
    {
        // The first argument that is neither an option nor an option's
        // value: git clone --depth 1 <url> <dir> works too.
        static constexpr std::string_view k_with_value[] = {"-b",
                                                            "--branch",
                                                            "-o",
                                                            "--origin",
                                                            "--depth",
                                                            "-c",
                                                            "--config",
                                                            "-j",
                                                            "--jobs",
                                                            "-u",
                                                            "--upload-pack",
                                                            "--reference",
                                                            "--template",
                                                            "--separate-git-dir",
                                                            "--filter",
                                                            "--shallow-since",
                                                            "--shallow-exclude"};
        std::string_view rest = s.substr(10), url;
        bool value_next = false;
        while (!rest.empty() && url.empty())
        {
            rest = trim(rest);
            const size_t space = rest.find(' ');
            const std::string_view token = rest.substr(0, space);
            rest = space == std::string_view::npos ? std::string_view() : rest.substr(space);
            if (value_next)
                value_next = false;
            else if (token.starts_with("-"))
                value_next = std::ranges::contains(k_with_value, token);
            else
                url = token;
        }
        s = url;
    }
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    const bool drive = s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) &&
                       s[1] == ':' && (s[2] == '\\' || s[2] == '/');
    for (const char* scheme : {"http://", "https://", "git://", "ssh://", "git@"})
        if (s.starts_with(scheme)) return std::string(s);
    return drive ? std::string(s) : std::string();
}

std::vector<std::vector<std::string>> save_remote_commands(const std::string& from,
                                                           const remote_settings& to)
{
    std::vector<std::vector<std::string>> commands;
    if (from.empty())
        commands.push_back({"remote", "add", "--", to.name, to.url});
    else
    {
        if (to.name != from) commands.push_back({"remote", "rename", "--", from, to.name});
        commands.push_back({"remote", "set-url", "--", to.name, to.url});
    }
    const std::string key = "remote." + to.name + ".";
    commands.push_back(to.push_url.empty()
                           ? std::vector<std::string>{"config", "--unset-all", key + "pushurl"}
                           : std::vector<std::string>{"config", key + "pushurl", to.push_url});
    commands.push_back(to.tag_opt.empty()
                           ? std::vector<std::string>{"config", "--unset", key + "tagOpt"}
                           : std::vector<std::string>{"config", key + "tagOpt", to.tag_opt});
    commands.push_back(to.prune ? std::vector<std::string>{"config", key + "prune", "true"}
                                : std::vector<std::string>{"config", "--unset", key + "prune"});
    return commands;
}

}  // namespace tortilla
