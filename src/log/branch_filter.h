// Which branches the log walks: the Show menu's preset (the current branch,
// all, local ones, pull requests', the user's own, or a set picked once),
// pins that are always shown, hide patterns that leave branches out, and
// two toggles that go with any of them (branch tips only; only what isn't
// merged).
#pragma once

#include "forge/pr_watch.h"
#include "git/repo.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct git_mailmap;

namespace tortilla
{

enum class show_preset
{
    current,        // HEAD, or the ref picked to start from
    all,            // every ref, like git log --all
    local,          // local branches
    pull_requests,  // open pull requests' heads and bases
    mine,           // the user's own: by their email, or their pull requests
    picked,         // a set picked from a list, for once
};

// The preset's name in settings ("current", "all", ...), and back; an
// unknown name is the current branch.
std::string_view preset_key(show_preset);
show_preset preset_from_key(std::string_view);

// What the Show menu has chosen.
struct branch_filter
{
    show_preset preset = show_preset::current;
    std::vector<std::string> picked;  // full names, for picked
    // Full names, and "HEAD" for what's checked out, whatever it is.
    std::vector<std::string> pins;
    std::vector<std::string> hidden;  // glob patterns over branch names
    bool tips_only = false;           // git log --simplify-by-decoration
    bool unmerged = false;            // each branch only as far as its merge base with main
};

// The pull request watch's branches, from the UI thread.
struct pr_branches
{
    bool known = false;  // the watch has answered
    // Heads and bases of the open pull requests, as remote branches' full
    // names; and the heads of those the signed-in account opened.
    std::vector<std::string> all, mine;
};

// What the log asks a load to walk: the filter, and what the forge knows.
struct branch_request
{
    branch_filter filter;
    pr_branches prs;
    std::vector<forge_remote> remotes;  // the forge remotes, for the leading one's main
};

// What a load made of it, for the status bar.
struct branch_outcome
{
    size_t pins_added = 0, hidden = 0;
    bool waiting = false;  // pull requests not known yet: the pins alone
    bool none = false;     // the preset found no branches: the pins alone
    std::string base;      // what "only what's not merged" stopped at, short
    bool no_base = false;  // it found nothing to stop at
};

// `*` matches any run of characters ('/' too), `?` any one.
bool glob_match(std::string_view pattern, std::string_view text);
// A branch a hide pattern matches: by its name ("dependabot/npm/x"), or a
// remote one's also without its remote ("origin/dependabot/npm/x").
bool hidden_by(const ref&, const std::vector<std::string>& patterns);
bool is_pinned(const ref&, const std::vector<std::string>& pins);

// Each remote's default branch, by full name: <remote>/HEAD's target, else
// its main, else its master; origin's first, then in the config's order.
// libgit2 lookups only, quick enough for the UI thread.
std::vector<std::string> remote_defaults(const repo&);
// What's pinned until the user says otherwise: HEAD, and each remote's
// default branch.
std::vector<std::string> default_pins(const repo&);

// The branch "only what's not merged" stops at: the leading forge remote's
// default branch, else a pinned remote default, else any remote's, else a
// local main or master; "" when there's none. `leading`: remote names,
// leading first.
std::string merge_base_branch(const repo&, const std::vector<std::string>& pins,
                              const std::vector<std::string>& leading);

// The commits a filter starts the walk from.
struct branch_choice
{
    std::vector<oid> shown;   // the preset's tips, walked in full
    std::vector<oid> pinned;  // pins' tips, shown only as far as the rest needs
    // The preset's branches, by full name (the current one has none: the
    // walk starts at HEAD or its ref).
    std::vector<std::string> shown_names;
    size_t pins_added = 0;  // pins that add to the preset, for the status
    size_t hidden = 0;      // branches the hide patterns left out
    bool waiting = false;   // pull requests, before the forge has answered
    bool none = false;      // the preset has no branches: the pins alone, in full
    // Labels to draw a pin on, by full name.
    std::unordered_set<std::string> pinned_names;
};

// `head`: HEAD's commit; `is_mine` whether a branch (at a commit) is the
// user's, for My branches (branch_is_mine).
branch_choice choose_branches(const branch_filter&, const ref_map&, const std::optional<oid>& head,
                              const pr_branches&,
                              const std::function<bool(const ref&, const oid&)>& is_mine);

// Who "me" is, for My branches: every user.email git sees (all scopes,
// includeIf too), what .mailmap makes of them, and the GitHub noreply
// addresses of the signed-in GitHub accounts (login@users.noreply.<host>,
// and <id>+login@ the same, any id: logins are unique). Reads git config
// and the mailmap: for a worker thread.
class identity
{
public:
    identity(const repo&, const std::vector<forge_account>& accounts);
    ~identity();
    identity(const identity&) = delete;
    identity& operator=(const identity&) = delete;
    // An author's email: one of mine, as given or through the mailmap.
    bool matches(std::string_view email) const;
    const std::vector<std::string>& emails() const { return emails_; }

private:
    std::vector<std::string> emails_;                           // lower case
    std::vector<std::pair<std::string, std::string>> noreply_;  // login, "@users.noreply.host"
    git_mailmap* mailmap_ = nullptr;
    bool listed(std::string_view lower) const;
};

// Whether a branch at `tip` is the user's: one of its own commits (those
// `base` hasn't got; up to a few thousand) is theirs; or it's a local
// branch they made here (its reflog starts "branch: Created from") that
// hasn't moved since.
bool branch_is_mine(const repo&, const ref&, const oid& tip, const std::optional<oid>& base,
                    const identity&);

// The open pull requests' branches for pr_branches: heads (the remote
// branches match_pull_requests found) and bases (into's remote and the
// base branch); `mine` those the account they're open with opened.
pr_branches pull_request_branches(const std::unordered_map<std::string, open_pr>& heads,
                                  const std::vector<open_pr>& prs, bool known);

}  // namespace tortilla
