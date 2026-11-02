#include "sync.h"

#include "git/command.h"
#include "i18n.h"
#include "settings.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tortilla
{
namespace
{

// The lines of `text`, without their \r.
template <class F>
void each_line(std::string_view text, F f)
{
    while (!text.empty())
    {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        f(line);
    }
}

// Splits at the first tab; the second part empty without one.
std::pair<std::string_view, std::string_view> at_tab(std::string_view s)
{
    const size_t tab = s.find('\t');
    if (tab == std::string_view::npos) return {s, {}};
    return {s.substr(0, tab), s.substr(tab + 1)};
}

bool is_hash(std::string_view s)
{
    return (s.size() == 40 || s.size() == 64) &&
           std::ranges::all_of(s,
                               [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

std::string_view when_word(sync_when w)
{
    switch (w)
    {
    case sync_when::always: return "always";
    case sync_when::unmetered: return "unmetered";
    case sync_when::never: return "never";
    }
    return "always";
}

sync_when when_setting(const std::string& key, sync_when fallback)
{
    const std::string v = settings::get().value(key, std::string(when_word(fallback)));
    for (sync_when w : {sync_when::always, sync_when::unmetered, sync_when::never})
        if (v == when_word(w)) return w;
    return fallback;
}

bool flag_setting(const std::string& key, bool fallback)
{
    return settings::get().value(key, fallback ? "1" : "0") == "1";
}

bool contains_any(std::string_view text, std::initializer_list<std::string_view> words)
{
    return std::ranges::any_of(words, [&](std::string_view w)
                               { return text.find(w) != std::string_view::npos; });
}

}  // namespace

sync_when prefetch_setting()
{
    return when_setting("sync.prefetch", sync_when::unmetered);
}

void set_prefetch_setting(sync_when w)
{
    settings::get().set("sync.prefetch", std::string(when_word(w)));
}

sync_when check_setting()
{
    return when_setting("sync.check", sync_when::always);
}

void set_check_setting(sync_when w)
{
    settings::get().set("sync.check", std::string(when_word(w)));
}

bool fast_forward_others_setting()
{
    return flag_setting("sync.fastForwardOthers", true);
}

void set_fast_forward_others_setting(bool on)
{
    settings::get().set("sync.fastForwardOthers", on ? "1" : "0");
}

bool fast_forward_current_setting()
{
    return flag_setting("sync.fastForwardCurrent", false);
}

void set_fast_forward_current_setting(bool on)
{
    settings::get().set("sync.fastForwardCurrent", on ? "1" : "0");
}

bool on_metered_network()
{
    return rap::sys::network_metered();
}

// The system's word (false only when it says there's no internet): a
// remote's failure then isn't the remote's.
bool network_online()
{
    return rap::sys::network_online();
}

bool allowed_now(sync_when w)
{
    return w == sync_when::always || (w == sync_when::unmetered && !on_metered_network());
}

remote_tips parse_ls_remote(std::string_view output)
{
    remote_tips out;
    each_line(output,
              [&](std::string_view line)
              {
                  const auto [sha, ref] = at_tab(line);
                  if (!is_hash(sha) || !ref.starts_with("refs/heads/")) return;
                  out.branches.push_back(
                      {.name = std::string(ref.substr(11)), .sha = std::string(sha)});
              });
    return out;
}

std::map<std::string, std::vector<branch_tip>, std::less<>>
local_tips(const std::string& workdir, const std::vector<std::string>& remotes,
           std::string_view prefix)
{
    std::map<std::string, std::vector<branch_tip>, std::less<>> out;
    if (remotes.empty()) return out;
    std::vector<std::string> args{"for-each-ref", "--format=%(objectname)%09%(refname)"};
    for (const std::string& r : remotes)
    {
        out[r];
        args.push_back(std::string(prefix) + r + "/");
    }
    const git_result listed = run_git(workdir, args, git_output::stdout_only);
    // A remote's name may hold a slash (team/fork): the longest that fits.
    std::vector<const std::string*> by_length;
    for (const std::string& r : remotes) by_length.push_back(&r);
    std::ranges::sort(by_length, std::greater<>{}, [](const std::string* r) { return r->size(); });
    each_line(listed.output,
              [&](std::string_view line)
              {
                  const auto [sha, full] = at_tab(line);
                  if (!full.starts_with(prefix)) return;
                  const std::string_view rest = full.substr(prefix.size());
                  for (const std::string* r : by_length)
                  {
                      if (rest.size() <= r->size() + 1 || !rest.starts_with(*r) ||
                          rest[r->size()] != '/')
                          continue;
                      const std::string_view name = rest.substr(r->size() + 1);
                      if (name != "HEAD")
                          out[*r].push_back({.name = std::string(name), .sha = std::string(sha)});
                      return;
                  }
              });
    return out;
}

remote_changes compare_tips(std::string remote, const remote_tips& theirs,
                            const std::vector<branch_tip>& ours)
{
    remote_changes out{.remote = std::move(remote)};
    std::unordered_map<std::string_view, std::string_view> mine;
    for (const branch_tip& b : ours) mine.emplace(b.name, b.sha);
    std::unordered_set<std::string_view> seen;
    for (const branch_tip& b : theirs.branches)
    {
        seen.insert(b.name);
        auto it = mine.find(b.name);
        if (it == mine.end())
            out.added.push_back(b.name);
        else if (it->second != b.sha)
            out.moved.push_back(b.name);
    }
    if (theirs.complete)
        for (const branch_tip& b : ours)
            if (!seen.contains(b.name)) out.deleted.push_back(b.name);
    std::ranges::sort(out.added);
    std::ranges::sort(out.moved);
    std::ranges::sort(out.deleted);
    return out;
}

std::string changes_text(const remote_changes& c)
{
    std::string parts;
    auto add = [&](const std::vector<std::string>& names, const char* what)
    {
        for (const std::string& n : names)
            parts += (parts.empty() ? "" : ", ") + rap::i18n::format(what, n);
    };
    add(c.moved, _("{0} moved"));
    add(c.added, _("{0} new"));
    add(c.deleted, _("{0} deleted"));
    return rap::i18n::format(_("{0}: {1}"), c.remote, parts);
}

std::vector<std::string> prefetch_args(const std::string& remote)
{
    return {"fetch",
            remote,
            "--prune",
            "--no-tags",
            "--no-write-fetch-head",
            "--recurse-submodules=no",
            "--refmap=",
            "--quiet",
            "+refs/heads/*:refs/prefetch/" + remote + "/*"};
}

std::vector<fast_forward> fast_forward_targets(const std::string& workdir,
                                               const std::string& remote)
{
    const std::string only = remote.empty() ? "" : "refs/remotes/" + remote + "/";
    std::vector<fast_forward> out;
    // Branches checked out anywhere stay: moving one would leave that
    // worktree's files behind its HEAD.
    std::unordered_set<std::string> checked_out;
    const git_result trees =
        run_git(workdir, {"worktree", "list", "--porcelain"}, git_output::stdout_only);
    each_line(trees.output,
              [&](std::string_view line)
              {
                  if (line.starts_with("branch ")) checked_out.emplace(line.substr(7));
              });
    // Every branch and remote branch's commit, then each local branch's
    // upstream and how it stands against it.
    const git_result tips = run_git(
        workdir,
        {"for-each-ref", "--format=%(refname)%09%(objectname)%09%(symref)", "refs/heads/",
         "refs/remotes/"},
        git_output::stdout_only);
    std::unordered_map<std::string_view, std::string_view> sha_of;
    std::unordered_set<std::string_view> remote_mains;  // what each origin/HEAD points at
    each_line(tips.output,
              [&](std::string_view line)
              {
                  const auto [full, rest] = at_tab(line);
                  const auto [sha, symref] = at_tab(rest);
                  sha_of.emplace(full, sha);
                  if (full.ends_with("/HEAD") && !symref.empty()) remote_mains.insert(symref);
              });
    const git_result branches =
        run_git(workdir,
                {"for-each-ref", "--format=%(refname)%09%(upstream)%09%(upstream:track)%09%(HEAD)",
                 "refs/heads/"},
                git_output::stdout_only);
    each_line(branches.output,
              [&](std::string_view line)
              {
                  auto [full, rest] = at_tab(line);
                  auto [upstream, rest2] = at_tab(rest);
                  auto [track, head] = at_tab(rest2);
                  // "[behind 3]" alone: "[ahead 1, behind 2]" has its own.
                  if (upstream.empty() || head == "*" || !track.starts_with("[behind ") ||
                      !upstream.starts_with(only) || checked_out.contains(std::string(full)))
                      return;
                  auto from = sha_of.find(full), to = sha_of.find(upstream);
                  if (from == sha_of.end() || to == sha_of.end()) return;
                  // A feature branch started from origin/main may track it
                  // (git's and the Track box's old default); once merged, it
                  // would be dragged up to main's tip on every fetch. So a
                  // remote's main moves only its namesake; a branch pushed
                  // under another name still follows that one.
                  const std::string_view name = full.substr(11);
                  const bool remote_main = remote_mains.contains(upstream) ||
                                           upstream.ends_with("/main") ||
                                           upstream.ends_with("/master");
                  const bool namesake = upstream.size() > name.size() &&
                                        upstream.ends_with(name) &&
                                        upstream[upstream.size() - name.size() - 1] == '/';
                  if (remote_main && !namesake) return;
                  out.push_back({.branch = std::string(name),
                                 .upstream = std::string(upstream),
                                 .from = std::string(from->second),
                                 .to = std::string(to->second)});
              });
    return out;
}

std::vector<std::vector<std::string>> fast_forward_commands(const std::vector<fast_forward>& ffs)
{
    std::vector<std::vector<std::string>> out;
    for (const fast_forward& f : ffs)
        out.push_back({"update-ref", "-m", "sync: fast-forward to " + f.upstream,
                       "refs/heads/" + f.branch, f.to, f.from});
    return out;
}

current_state current_against_upstream(const std::string& workdir)
{
    current_state out;
    auto trimmed = [](std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    };
    const git_result branch =
        run_git(workdir, {"symbolic-ref", "-q", "--short", "HEAD"}, git_output::stdout_only);
    if (!branch.ok()) return out;
    out.branch = trimmed(branch.output);
    const git_result up =
        run_git(workdir, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"},
                git_output::stdout_only);
    if (!up.ok()) return out;
    out.upstream = trimmed(up.output);
    const git_result counts = run_git(
        workdir, {"rev-list", "--left-right", "--count", "HEAD...@{u}"}, git_output::stdout_only);
    const std::string both = trimmed(counts.output);
    const auto [ahead, behind] = at_tab(both);
    std::from_chars(ahead.data(), ahead.data() + ahead.size(), out.ahead);
    std::from_chars(behind.data(), behind.data() + behind.size(), out.behind);
    if (out.behind > 0)
    {
        const git_result status = run_git(
            workdir, {"status", "--porcelain", "--untracked-files=no"}, git_output::stdout_only);
        out.clean = status.ok() && status.output.empty();
    }
    return out;
}

remote_result classify_failure(std::string_view output)
{
    remote_result out{.failure = fetch_failure::other, .output = std::string(output)};
    std::string lower(output);
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (contains_any(lower, {"could not resolve host", "could not resolve hostname",
                             "temporary failure in name resolution", "name or service not known",
                             "network is unreachable", "no route to host", "connection timed out",
                             "operation timed out", "connection refused", "failed to connect",
                             "couldn't connect to server", "connection reset"}))
    {
        out.failure = fetch_failure::unreachable;
        out.why = _("host unreachable");
        return out;
    }
    if (contains_any(lower,
                     {"authentication failed", "could not read username", "could not read password",
                      "terminal prompts disabled", "permission denied (publickey",
                      "host key verification failed", "invalid username or password"}))
    {
        out.failure = fetch_failure::credentials;
        out.why = _("needs a password; use Fetch... to sign in");
        return out;
    }
    if (contains_any(lower, {"repository not found", "does not appear to be a git repository"}))
    {
        out.why = _("repository not found");
        return out;
    }
    // git's own words: its first fatal line, else its first.
    std::string_view first, fatal;
    each_line(output,
              [&](std::string_view line)
              {
                  if (first.empty() && !line.empty()) first = line;
                  if (fatal.empty() && line.starts_with("fatal: ")) fatal = line.substr(7);
              });
    out.why = std::string(fatal.empty() ? first : fatal);
    if (out.why.empty()) out.why = _("the fetch failed");
    return out;
}

std::vector<std::string> fetch_all_remotes(const std::string& workdir)
{
    std::vector<std::string> out;
    const git_result names = run_git(workdir, {"remote"}, git_output::stdout_only);
    const git_result skipped =
        run_git(workdir, {"config", "--type=bool", "--get-regexp", "^remote\\..*\\.skipfetchall$"},
                git_output::stdout_only);
    each_line(names.output,
              [&](std::string_view name)
              {
                  if (name.empty()) return;
                  bool skip = false;
                  each_line(skipped.output,
                            [&](std::string_view line)
                            {
                                // remote.<name>.skipfetchall true
                                const std::string key =
                                    "remote." + std::string(name) + ".skipfetchall ";
                                if (line.starts_with(key))
                                    skip = skip || line.substr(key.size()) == "true";
                            });
                  if (!skip) out.emplace_back(name);
              });
    return out;
}

std::vector<std::string> fetch_args(const std::string& remote)
{
    // --no-write-fetch-head and --no-auto-maintenance came in git 2.29.
    static const bool modern = []
    {
        const git_result v = run_git(".", {"version"}, git_output::stdout_only);
        // "git version 2.55.0"
        const size_t at = v.output.find_first_of("0123456789");
        int major = 0, minor = 0;
        if (at != std::string::npos)
        {
            const char* p = v.output.data() + at;
            const char* end = v.output.data() + v.output.size();
            p = std::from_chars(p, end, major).ptr;
            if (p < end && *p == '.') std::from_chars(p + 1, end, minor);
        }
        return major > 2 || (major == 2 && minor >= 29);
    }();
    if (modern)
        return {"fetch", "--prune", "--no-write-fetch-head", "--no-auto-maintenance", remote};
    return {"-c", "maintenance.auto=false", "-c", "gc.auto=0", "fetch", "--prune", remote};
}

remote_result fetch_remote(const std::string& workdir, const std::string& remote)
{
    const std::vector<std::string> args = fetch_args(remote);
    const git_result fetched = run_git(workdir, args, git_output::all, git_prompts::never);
    remote_result result = fetched.ok() ? remote_result{} : classify_failure(fetched.output);
    result.output = command_line(args) + "\n" + fetched.output;
    return result;
}

std::map<std::string, remote_result, std::less<>>
fetch_remotes(const std::string& workdir, const std::vector<std::string>& remotes)
{
    std::vector<remote_result> results(remotes.size());
    {
        std::vector<std::jthread> each;
        for (size_t i = 0; i < remotes.size(); ++i)
            each.emplace_back([&, i] { results[i] = fetch_remote(workdir, remotes[i]); });
    }
    std::map<std::string, remote_result, std::less<>> out;
    for (size_t i = 0; i < remotes.size(); ++i) out[remotes[i]] = std::move(results[i]);
    return out;
}

}  // namespace tortilla
