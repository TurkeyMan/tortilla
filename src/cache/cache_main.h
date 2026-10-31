// tortilla cache: the status cache behind the file manager badges, one per
// user, started by the extension when it finds none running. It serves
// "badge <path>" with the badge's number (tortilla::badge), and "state
// <path>" with badge_cache::state's ("<badge> <current> <untracked>"), on
// the channel named k_cache_channel; it ends after a while with no
// questions.
#pragma once

#include <string>
#include <string_view>

namespace tortilla
{

class badge_cache;

inline constexpr const char* k_cache_channel = "cache";

// The answer to one request line.
std::string answer_cache_request(badge_cache&, std::string_view request);

int run_cache();

}  // namespace tortilla
