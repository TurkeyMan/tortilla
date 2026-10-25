// Reflogs, for the RefLog dialog (TortoiseGit's CRefLogDlg): which refs have
// one, and a ref's entries, newest first. Read through libgit2.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tortilla
{

class repo;

struct reflog_entry
{
    std::string selector;  // as git names it: "HEAD@{0}", "refs/heads/main@{2}", "stash@{1}"
    std::string id;        // the ref's commit after this change
    std::string old_id;    // before it; "" for the entry that created the ref
    std::string action;    // "commit", "checkout", "reset", "rebase (finish)"...
    std::string message;   // the rest: "moving from main to topic"
    std::string committer;
    int64_t time = 0;
};

// The refs with a reflog: HEAD first, then local branches, remote branches,
// the stash and the rest, each in name order.
std::vector<std::string> reflog_refs(const repo&);

// `ref`'s reflog (a full name, or HEAD), newest first.
std::expected<std::vector<reflog_entry>, std::string> read_reflog(const repo&,
                                                                  const std::string& ref);

// "checkout: moving from a to b" as {"checkout", "moving from a to b"}. A
// stash's message ("WIP on main: 1a2b3c4 subject") has no action.
std::pair<std::string, std::string> split_reflog_message(std::string_view message, bool stash);

// The name a selector is shown and given to git stash by: "stash@{1}" for
// refs/stash, "main@{2}" for refs/heads/main, HEAD's as it is.
std::string short_selector(const std::string& ref, size_t index);

}  // namespace tortilla
