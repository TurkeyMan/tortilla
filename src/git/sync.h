// The log's sync (its Fetch button): what the remotes have that the
// repository hasn't yet, a fetch into a private namespace ahead of the
// click, and which local branches a sync can move forward. Git queries and
// fetches: for a worker thread, except the settings.
#pragma once

#include "forge/forge.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

class settings_changes;  // settings.h

// -- settings -----------------------------------------------------------------

// When to do something that uses the network unasked.
enum class sync_when
{
    always,
    unmetered,  // not on a metered connection
    never,
};
// Downloading remote changes in the background (the prefetch below).
sync_when prefetch_setting();
void set_prefetch_setting(sync_when);
void set_prefetch_setting(settings_changes&, sync_when);
// Asking the forge (or the remote, with ls-remote) what has changed.
sync_when check_setting();
void set_check_setting(sync_when);
void set_check_setting(settings_changes&, sync_when);
// Moving forward branches that aren't checked out; on by default.
bool fast_forward_others_setting();
void set_fast_forward_others_setting(bool);
void set_fast_forward_others_setting(settings_changes&, bool);
// Moving the checked-out branch forward when the working tree is clean;
// off by default.
bool fast_forward_current_setting();
void set_fast_forward_current_setting(bool);
void set_fast_forward_current_setting(settings_changes&, bool);

// Whether the connection is metered (a phone's hotspot, a capped plan).
bool on_metered_network();
// Whether the system says it's connected to a network at all: without, the
// Fetch button is Offline rather than a remote's failure.
bool network_online();
// Whether `when` allows it now.
bool allowed_now(sync_when);

// -- what the remotes have ----------------------------------------------------

// `git ls-remote --heads` output: a branch per line, "<sha>\trefs/heads/<name>".
// Anything else (git's warnings on stderr) is skipped.
remote_tips parse_ls_remote(std::string_view output);

// The repository's own copy of `remotes`' branches: refs/remotes/<remote>/*
// (or under `prefix`: refs/prefetch/), by remote, without <remote>/HEAD.
// Every remote asked for has an entry, empty if it has no branches.
std::map<std::string, std::vector<branch_tip>, std::less<>>
local_tips(const std::string& workdir, const std::vector<std::string>& remotes,
           std::string_view prefix = "refs/remotes/");

// What a sync would bring from one remote: its branches that are new, have
// moved, or are gone, by name. Gone ones only from a complete list.
struct remote_changes
{
    std::string remote;
    std::vector<std::string> added, moved, deleted;
    bool empty() const { return added.empty() && moved.empty() && deleted.empty(); }
};
remote_changes compare_tips(std::string remote, const remote_tips& theirs,
                            const std::vector<branch_tip>& ours);
// "origin: main moved, ow/tx-floor new, old-thing deleted".
std::string changes_text(const remote_changes&);

// The fetch that downloads a remote's branches ahead of a sync, into
// refs/prefetch/<remote>/ as git maintenance does: refs/remotes/ stays as
// it is (an empty --refmap), and no tags (they'd be the repository's).
std::vector<std::string> prefetch_args(const std::string& remote);

// -- after the fetch ----------------------------------------------------------

// A local branch a sync can fast-forward: not checked out in any worktree,
// tracking an upstream it's strictly behind (no commits of its own).
struct fast_forward
{
    std::string branch;    // short name
    std::string upstream;  // full ref name
    std::string from, to;  // hashes
};
// `remote`: only branches whose upstream is that remote's (refs/remotes/<remote>/).
std::vector<fast_forward> fast_forward_targets(const std::string& workdir,
                                               const std::string& remote = {});
// update-ref commands that make them so, each checking the branch is still
// where it was.
std::vector<std::vector<std::string>> fast_forward_commands(const std::vector<fast_forward>&);

// The checked-out branch against its upstream.
struct current_state
{
    std::string branch;    // "" when detached
    std::string upstream;  // short: origin/main; "" for none
    int ahead = 0, behind = 0;
    bool clean = false;  // no changes to tracked files
};
current_state current_against_upstream(const std::string& workdir);

// -- when a remote can't be reached -------------------------------------------

enum class fetch_failure
{
    none,
    unreachable,  // the host, or the network
    credentials,  // it wants a password or a key it can't be given unasked
    other,
};
struct remote_result
{
    fetch_failure failure = fetch_failure::none;
    std::string why;     // "host unreachable"; "" when it worked
    std::string output;  // git's output for this remote
};
// Why a fetch (or ls-remote) failed, from git's output.
remote_result classify_failure(std::string_view output);
// The remotes `git fetch --all` fetches: all but those set to be skipped
// (remote.<name>.skipFetchAll).
std::vector<std::string> fetch_all_remotes(const std::string& workdir);
// The fetch of one remote for a sync: pruned, and leaving FETCH_HEAD and
// git's auto maintenance alone, so remotes fetched side by side don't
// contend (as -c options for a git older than 2.29).
std::vector<std::string> fetch_args(const std::string& remote);
// `git fetch` of one remote, never asking: its result on its own, which
// --all's output (in the user's language) can't be split into reliably.
remote_result fetch_remote(const std::string& workdir, const std::string& remote);
// The remotes fetched side by side, a git each: their results.
std::map<std::string, remote_result, std::less<>> fetch_remotes(const std::string& workdir,
                                                                const std::vector<std::string>&);

}  // namespace tortilla
