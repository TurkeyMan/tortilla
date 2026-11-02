#include "branch_filter.h"

#include "git/command.h"

#include <git2.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace tortilla
{
namespace
{

constexpr std::pair<show_preset, std::string_view> k_preset_keys[] = {
    {show_preset::current, "current"}, {show_preset::all, "all"},
    {show_preset::local, "local"},     {show_preset::pull_requests, "pulls"},
    {show_preset::mine, "mine"},       {show_preset::picked, "picked"},
};

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = char(std::tolower((unsigned char)c));
    return out;
}

// A branch's own commits are walked this far at most: further, it's
// unrelated history (another root), not a branch off main.
constexpr size_t k_own_commits_limit = 5000;

bool is_branch(const ref& r)
{
    return r.kind == ref_kind::local_branch || r.kind == ref_kind::remote_branch;
}

bool ref_exists(git_repository* r, const std::string& full_name)
{
    git_oid id;
    return git_reference_name_to_id(&id, r, full_name.c_str()) == 0;
}

}  // namespace

std::string_view preset_key(show_preset p)
{
    for (auto [preset, key] : k_preset_keys)
        if (preset == p) return key;
    return "current";
}

show_preset preset_from_key(std::string_view key)
{
    for (auto [preset, k] : k_preset_keys)
        if (k == key) return preset;
    return show_preset::current;
}

bool glob_match(std::string_view pattern, std::string_view text)
{
    // The usual backtracking: the last `*` takes one more character each
    // time the rest fails to match.
    size_t p = 0, t = 0, star = std::string_view::npos, resume = 0;
    while (t < text.size())
    {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
        {
            ++p;
            ++t;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            resume = t;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            t = ++resume;
        }
        else
            return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool hidden_by(const ref& r, const std::vector<std::string>& patterns)
{
    if (!is_branch(r)) return false;
    std::string_view name = r.name, bare = name;
    if (r.kind == ref_kind::remote_branch)
        if (size_t slash = name.find('/'); slash != std::string_view::npos)
            bare = name.substr(slash + 1);
    return std::ranges::any_of(patterns, [&](const std::string& p)
                               { return glob_match(p, name) || glob_match(p, bare); });
}

bool is_pinned(const ref& r, const std::vector<std::string>& pins)
{
    for (const std::string& p : pins)
        if (p == r.full_name || (p == "HEAD" && (r.current || r.kind == ref_kind::head)))
            return true;
    return false;
}

std::vector<std::string> remote_defaults(const repo& repo)
{
    std::vector<std::string> out;
    git_strarray names{};
    if (git_remote_list(&names, repo.raw()) != 0) return out;
    std::vector<std::string> remotes(names.strings, names.strings + names.count);
    git_strarray_dispose(&names);
    std::ranges::stable_partition(remotes, [](const std::string& r) { return r == "origin"; });
    for (const std::string& remote : remotes)
    {
        const std::string prefix = "refs/remotes/" + remote + "/";
        std::vector<std::string> candidates;
        git_reference* head = nullptr;
        if (git_reference_lookup(&head, repo.raw(), (prefix + "HEAD").c_str()) == 0)
        {
            if (const char* target = git_reference_symbolic_target(head))
                candidates.push_back(target);
            git_reference_free(head);
        }
        candidates.push_back(prefix + "main");
        candidates.push_back(prefix + "master");
        for (const std::string& c : candidates)
            if (ref_exists(repo.raw(), c))
            {
                out.push_back(c);
                break;
            }
    }
    return out;
}

identity::identity(const repo& repo, const std::vector<forge_account>& accounts)
{
    git_mailmap_from_repository(&mailmap_, repo.raw());
    // Every scope's (git sees includeIf's too, as libgit2 may not).
    const git_result r = run_git(repo.workdir().empty() ? repo.git_dir() : repo.workdir(),
                                 {"config", "--get-all", "user.email"}, git_output::stdout_only);
    for (std::string_view rest = r.output; !rest.empty();)
    {
        const size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty()) continue;
        emails_.push_back(lower(line));
        // And what the mailmap makes of it, so its other addresses match.
        const char *name = nullptr, *email = nullptr;
        const std::string given(line);
        if (mailmap_ && git_mailmap_resolve(&name, &email, mailmap_, "", given.c_str()) == 0 &&
            email)
            emails_.push_back(lower(email));
    }
    std::ranges::sort(emails_);
    emails_.erase(std::ranges::unique(emails_).begin(), emails_.end());
    for (const forge_account& a : accounts)
        if (a.kind == forge_kind::github && !a.login.empty())
            noreply_.emplace_back(lower(a.login), "@users.noreply." + lower(a.host));
}

identity::~identity()
{
    if (mailmap_) git_mailmap_free(mailmap_);
}

bool identity::listed(std::string_view email) const
{
    if (std::ranges::binary_search(emails_, email)) return true;
    // login@users.noreply.github.com, or 12345+login@ the same.
    for (const auto& [login, domain] : noreply_)
    {
        if (!email.ends_with(domain)) continue;
        std::string_view user = email.substr(0, email.size() - domain.size());
        if (user == login) return true;
        if (const size_t plus = user.find('+');
            plus != std::string_view::npos && plus > 0 && user.substr(plus + 1) == login &&
            std::ranges::all_of(user.substr(0, plus), [](char c) { return c >= '0' && c <= '9'; }))
            return true;
    }
    return false;
}

bool identity::matches(std::string_view email) const
{
    const std::string given = lower(email);
    if (listed(given)) return true;
    const char *name = nullptr, *mapped = nullptr;
    const std::string raw(email);
    return mailmap_ && git_mailmap_resolve(&name, &mapped, mailmap_, "", raw.c_str()) == 0 &&
           mapped && listed(lower(mapped));
}

bool branch_is_mine(const repo& repo, const ref& r, const oid& tip, const std::optional<oid>& base,
                    const identity& me)
{
    git_revwalk* walk = nullptr;
    if (git_revwalk_new(&walk, repo.raw()) != 0) return false;
    git_oid t, b;
    std::memcpy(t.id, tip.bytes.data(), tip.bytes.size());
    git_revwalk_push(walk, &t);
    if (base)
    {
        std::memcpy(b.id, base->bytes.data(), base->bytes.size());
        git_revwalk_hide(walk, &b);
    }
    bool mine = false;
    git_oid id;
    for (size_t n = 0; !mine && n < k_own_commits_limit && git_revwalk_next(&id, walk) == 0; ++n)
    {
        git_commit* c = nullptr;
        if (git_commit_lookup(&c, repo.raw(), &id) != 0) continue;
        if (const git_signature* a = git_commit_author(c); a && a->email)
            mine = me.matches(a->email);
        git_commit_free(c);
    }
    git_revwalk_free(walk);
    if (mine || r.kind != ref_kind::local_branch) return mine;

    // Made here by me, and not moved since: its reflog's first entry.
    git_reflog* log = nullptr;
    if (git_reflog_read(&log, repo.raw(), r.full_name.c_str()) != 0) return false;
    if (const size_t n = git_reflog_entrycount(log); n > 0)
    {
        const git_reflog_entry* first = git_reflog_entry_byindex(log, n - 1);  // the oldest
        const char* message = git_reflog_entry_message(first);
        const git_signature* who = git_reflog_entry_committer(first);
        mine = message && std::string_view(message).starts_with("branch: Created from") && who &&
               who->email && me.matches(who->email) &&
               std::memcmp(git_reflog_entry_id_new(first)->id, tip.bytes.data(),
                           tip.bytes.size()) == 0;
    }
    git_reflog_free(log);
    return mine;
}

std::vector<std::string> default_pins(const repo& repo)
{
    std::vector<std::string> out{"HEAD"};
    std::ranges::move(remote_defaults(repo), std::back_inserter(out));
    return out;
}

std::string merge_base_branch(const repo& repo, const std::vector<std::string>& pins,
                              const std::vector<std::string>& leading)
{
    const std::vector<std::string> defaults = remote_defaults(repo);
    for (const std::string& remote : leading)
        for (const std::string& d : defaults)
            if (d.starts_with("refs/remotes/" + remote + "/")) return d;
    for (const std::string& p : pins)
        if (std::ranges::find(defaults, p) != defaults.end()) return p;
    if (!defaults.empty()) return defaults.front();
    for (const char* local : {"refs/heads/main", "refs/heads/master"})
        if (ref_exists(repo.raw(), local)) return local;
    return {};
}

branch_choice choose_branches(const branch_filter& f, const ref_map& refs,
                              const std::optional<oid>& head, const pr_branches& prs,
                              const std::function<bool(const ref&, const oid&)>& is_mine)
{
    branch_choice out;
    const std::unordered_set<std::string> pr_all(prs.all.begin(), prs.all.end()),
        pr_mine(prs.mine.begin(), prs.mine.end()), picked(f.picked.begin(), f.picked.end());
    std::unordered_map<std::string_view, oid> by_name;
    std::unordered_set<oid, oid_hash> shown;
    auto show = [&](const oid& id, const ref& r)
    {
        out.shown_names.push_back(r.full_name);
        if (shown.insert(id).second) out.shown.push_back(id);
    };
    if (f.preset == show_preset::pull_requests && !prs.known) out.waiting = true;

    for (const auto& [id, labels] : refs)
        for (const ref& r : labels)
        {
            by_name.emplace(r.full_name, id);
            const bool pinned = is_pinned(r, f.pins);
            if (pinned) out.pinned_names.insert(r.full_name);
            const bool hidden = !pinned && hidden_by(r, f.hidden);
            if (hidden) ++out.hidden;
            if (f.preset == show_preset::picked)
            {
                if (picked.contains(r.full_name)) show(id, r);
                continue;
            }
            if (hidden) continue;
            bool wanted = false;
            switch (f.preset)
            {
            case show_preset::current:
            case show_preset::picked: break;
            case show_preset::all: wanted = true; break;
            case show_preset::local: wanted = r.kind == ref_kind::local_branch; break;
            case show_preset::pull_requests:
                // The remote branches, and local ones that track them.
                wanted = pr_all.contains(r.full_name) ||
                         (r.kind == ref_kind::local_branch && pr_all.contains(r.upstream));
                break;
            case show_preset::mine:
                // My pull requests' heads (and local branches tracking
                // them), and branches with my own commits or made by me.
                wanted = is_branch(r) &&
                         (pr_mine.contains(r.full_name) ||
                          (r.kind == ref_kind::local_branch && pr_mine.contains(r.upstream)) ||
                          (is_mine && is_mine(r, id)));
                break;
            }
            if (wanted) show(id, r);
        }
    std::ranges::sort(out.shown_names);

    // Pins the preset doesn't already show. The current branch's walk
    // starts at HEAD, which has it.
    std::unordered_set<oid, oid_hash> pinned;
    for (const std::string& p : f.pins)
    {
        std::optional<oid> id;
        if (p == "HEAD")
            id = head;
        else if (auto it = by_name.find(p); it != by_name.end())
            id = it->second;
        if (!id) continue;
        const bool in_preset = f.preset == show_preset::current
                                   ? p == "HEAD" || (head && *id == *head)
                               : p == "HEAD" ? f.preset == show_preset::all || shown.contains(*id)
                                             : std::ranges::binary_search(out.shown_names, p);
        if (in_preset) continue;
        ++out.pins_added;
        if (!shown.contains(*id) && pinned.insert(*id).second) out.pinned.push_back(*id);
    }
    // Nothing else to show (no pull requests yet, say): the pins in full.
    if (out.shown.empty() && f.preset != show_preset::current)
    {
        out.none = true;
        std::swap(out.shown, out.pinned);
    }
    return out;
}

pr_branches pull_request_branches(const std::unordered_map<std::string, open_pr>& heads,
                                  const std::vector<open_pr>& prs, bool known)
{
    pr_branches out{.known = known};
    for (const auto& [name, p] : heads)
    {
        out.all.push_back(name);
        if (!p.pr.author.empty() && p.pr.author == p.into.account.login) out.mine.push_back(name);
    }
    for (const open_pr& p : prs)
        if (!p.pr.base_branch.empty())
            out.all.push_back("refs/remotes/" + p.into.name + "/" + p.pr.base_branch);
    std::ranges::sort(out.all);
    std::ranges::sort(out.mine);
    out.all.erase(std::ranges::unique(out.all).begin(), out.all.end());
    return out;
}

}  // namespace tortilla
