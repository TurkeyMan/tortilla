// What the log's forge panel shows of a pull request beyond the list's
// summary: its description and reviews, asked for only when it's shown;
// which commits are its own; and its reviewers, summed up.
#pragma once

#include "forge/pr_watch.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

// The description and reviews of one pull request, for those the list
// leaves out (pull_request::body_known, reviews_known): GitHub's
// description (one request), GitLab's approvals (one), Gitea's reviews
// (one). `pr` comes back with them filled in.
std::expected<pull_request, std::string>
pull_request_details(forge_kind, const forge_repo& into, pull_request pr, std::string_view token);

// Who approved, who asked for changes, who commented, and who's still
// asked to review, each once, by their latest say. A comment after an
// approval or a request for changes doesn't undo it; being asked again
// does (GitHub's rule).
struct review_summary
{
    std::vector<std::string> approved, changes_requested, commented, waiting;
};
review_summary summarise_reviews(const std::vector<pr_review>&);

// The commits a pull request brings: reachable from its head's
// remote-tracking ref and not from its base's, newest first, at most
// `limit`. Empty when either ref is missing. A git run: for a worker.
std::vector<std::string> pull_request_commits(const std::string& workdir,
                                              const std::string& head_ref,
                                              const std::string& base_ref, size_t limit = 1000);

// The first of `remotes` (in order) with a remote-tracking ref containing
// `sha`: where the commit was pushed, so a forge can say what CI made of it.
// nullptr when it's only here. A git run: for a worker.
const forge_remote* remote_with_commit(const std::string& workdir, std::string_view sha,
                                       const std::vector<forge_remote>& remotes);

}  // namespace tortilla
