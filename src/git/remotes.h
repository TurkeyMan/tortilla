// What the remote dialogs (Clone, Fetch, Pull, Push) read from a
// repository [spec 7]: its remotes, branches and their upstreams. Quick git
// queries, for the UI thread.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace tortilla
{

// `git remote`, in its order.
std::vector<std::string> remote_names(const std::string& workdir);
// A config value, trimmed; "" if unset.
std::string config_value(const std::string& workdir, const std::string& key);
// The checked-out branch's short name; "" when HEAD is detached.
std::string current_branch(const std::string& workdir);
// Local branches' short names.
std::vector<std::string> local_branches(const std::string& workdir);
// A revision's hash; "" if it doesn't resolve.
std::string resolve(const std::string& workdir, const std::string& rev);
// Whether `ancestor` is an ancestor of (or is) `of`.
bool is_ancestor(const std::string& workdir, const std::string& ancestor, const std::string& of);
bool is_shallow(const std::string& workdir);

// A branch's upstream on its remote: branch.<b>.remote and the branch of
// branch.<b>.merge (without refs/heads/). Empty when it has none.
struct upstream
{
    std::string remote, branch;
};
upstream upstream_of(const std::string& workdir, const std::string& branch);

// Where a branch pushes: branch.<b>.pushremote, remote.pushdefault, then
// branch.<b>.remote [spec 7.3.2]; "" if none says.
std::string push_remote_of(const std::string& workdir, const std::string& branch);

// The folder a clone of `url` makes by default: its last component without
// ".git" [spec 7.1.2].
std::string clone_folder_name(std::string url);

}  // namespace tortilla
