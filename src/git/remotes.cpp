#include "remotes.h"

#include "git/command.h"

#include <algorithm>
#include <cctype>

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

}  // namespace

std::vector<std::string> remote_names(const std::string& workdir)
{
    const git_result r = run_git(workdir, {"remote"}, git_output::stdout_only);
    return r.ok() ? lines(r.output) : std::vector<std::string>{};
}

std::string config_value(const std::string& workdir, const std::string& key)
{
    return query(workdir, {"config", "--get", key});
}

std::string current_branch(const std::string& workdir)
{
    return query(workdir, {"symbolic-ref", "--short", "-q", "HEAD"});
}

std::vector<std::string> local_branches(const std::string& workdir)
{
    const git_result r =
        run_git(workdir, {"for-each-ref", "--format=%(refname:short)", "refs/heads"},
                git_output::stdout_only);
    return r.ok() ? lines(r.output) : std::vector<std::string>{};
}

std::string resolve(const std::string& workdir, const std::string& rev)
{
    return query(workdir, {"rev-parse", "-q", "--verify", "--end-of-options", rev + "^{commit}"});
}

bool is_ancestor(const std::string& workdir, const std::string& ancestor, const std::string& of)
{
    return run_git(workdir, {"merge-base", "--is-ancestor", ancestor, of}, git_output::stdout_only)
        .ok();
}

bool is_shallow(const std::string& workdir)
{
    return query(workdir, {"rev-parse", "--is-shallow-repository"}) == "true";
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

}  // namespace tortilla
