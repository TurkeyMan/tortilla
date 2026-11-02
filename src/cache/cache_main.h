// tortilla cache: the status cache behind the file manager badges, one per
// user, started by the extension when it finds none running. It serves
// "badge <path>" with the badge's number (tortilla::badge), and
// "states<TAB><path><TAB><path>..." with each one's badge_cache::state
// ("<badge> <current> <untracked>", ';' between), and "changes ..." with
// the paths whose badges changed (badge_changes.h), on the channel named
// k_cache_channel; it ends after a while with no questions.
#pragma once

#include <string>
#include <string_view>

namespace tortilla
{

class badge_cache;
class badge_changes;

inline constexpr const char* k_cache_channel = "cache";

// The answer to one request line; "changes" needs `changes`.
std::string answer_cache_request(badge_cache&, std::string_view request,
                                 const badge_changes* changes = nullptr);

int run_cache();

}  // namespace tortilla
