// SHA-256, for the few hashes Tortilla makes itself (Gravatar's email
// hash); git's object ids come from libgit2.
#pragma once

#include <string>
#include <string_view>

namespace tortilla
{

// The digest of `data` as 64 lowercase hex digits.
std::string sha256_hex(std::string_view data);

}  // namespace tortilla
