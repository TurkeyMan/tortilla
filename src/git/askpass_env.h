// The environment that makes the git tortilla runs, and the ssh it runs,
// ask their questions (passwords, passphrases, unknown hosts) through
// tortilla itself: GIT_ASKPASS and SSH_ASKPASS name tortilla, and
// TORTILLA_ASKPASS tells it why it was started (ui/askpass.h).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tortilla
{

inline constexpr const char* k_askpass_marker = "TORTILLA_ASKPASS";

std::vector<std::pair<std::string, std::string>> askpass_environment();

}  // namespace tortilla
