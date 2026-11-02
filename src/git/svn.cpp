#include "svn.h"

#include "git/command.h"

#include <mutex>
#include <optional>
#include <set>

namespace tortilla
{

bool git_svn_installed(const std::string& dir)
{
    static std::mutex mutex;
    static std::optional<bool> known;
    {
        std::lock_guard lock(mutex);
        if (known) return *known;
    }
    const bool ok = run_git(dir, {"svn", "--version"}, git_output::all, git_prompts::never).ok();
    std::lock_guard lock(mutex);
    known = ok;
    return ok;
}

bool in_svn(std::string_view message)
{
    // A line of its own at the message's end, as git svn writes it.
    size_t at = 0;
    while ((at = message.find("git-svn-id: ", at)) != std::string_view::npos)
    {
        if (at == 0 || message[at - 1] == '\n') return true;
        ++at;
    }
    return false;
}

std::string svn_upstream(std::string_view fetch)
{
    // The first line, if several.
    fetch = fetch.substr(0, fetch.find('\n'));
    while (!fetch.empty() && (fetch.back() == '\r' || fetch.back() == ' ')) fetch.remove_suffix(1);
    const size_t colon = fetch.find(':');
    if (colon == std::string_view::npos) return {};
    std::string_view ref = fetch.substr(colon + 1);
    if (ref.starts_with("refs/")) ref.remove_prefix(5);
    return std::string(ref);
}

std::vector<std::string> svn_dcommit_args(bool rmdir)
{
    std::vector<std::string> args{"svn", "dcommit"};
    if (rmdir) args.push_back("--rmdir");
    return args;
}

std::vector<std::string> svn_fetch_args()
{
    return {"svn", "fetch"};
}

std::vector<std::string> svn_ignore_args(svn_ignore_into into)
{
    return {"svn", into == svn_ignore_into::exclude ? "show-ignore" : "create-ignore"};
}

namespace
{

// Each line of `text`, without its line ending; blank ones skipped.
template <class F>
void each_line(std::string_view text, F&& f)
{
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) f(line);
        at = end + 1;
    }
}

}  // namespace

std::string merge_exclude(std::string_view existing, std::string_view ignores)
{
    std::set<std::string_view> have;
    each_line(existing, [&](std::string_view line) { have.insert(line); });
    std::string out(existing);
    bool ended = out.empty() || out.back() == '\n';
    each_line(ignores,
              [&](std::string_view line)
              {
                  if (!have.insert(line).second) return;
                  if (!ended) out += '\n';
                  out.append(line);
                  out += '\n';
                  ended = true;
              });
    return out;
}

}  // namespace tortilla
