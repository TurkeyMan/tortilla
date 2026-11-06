// The remote dropdown shared by Push and Pull/Fetch.
#pragma once

#include "git/remotes.h"
#include "i18n.h"

#include <rapport/rapport.h>

namespace tortilla
{

inline constexpr const char* all_remotes_label = T_("- all -");

inline int first_named_remote(const std::vector<std::string>& names, bool allow_all)
{
    return allow_all && names.size() > 1 ? 1 : 0;
}

inline std::vector<std::string> remote_items(const std::vector<std::string>& names, bool allow_all)
{
    std::vector<std::string> items;
    if (first_named_remote(names, allow_all)) items.push_back(_(all_remotes_label));
    items.insert(items.end(), names.begin(), names.end());
    items.push_back(_("Arbitrary URL..."));
    return items;
}

// Restore the same named remote, All or URL after Settings changes the list.
inline void refresh_remote_selector(rap::drop_down& selector, std::vector<std::string>& names,
                                    const std::string& workdir, bool allow_all)
{
    const int old_first = first_named_remote(names, allow_all);
    const int was = selector.current() - old_first;
    const bool url = was == int(names.size());
    const bool all = old_first == 1 && selector.current() == 0;
    const std::string name = was >= 0 && was < int(names.size()) ? names[size_t(was)] : "";

    names = remote_names(workdir);
    const auto items = remote_items(names, allow_all);
    selector.set_items(items);
    const int first = first_named_remote(names, allow_all);
    int current = names.empty() || url ? int(items.size()) - 1 : first;
    if (all && first == 1) current = 0;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) current = first + int(i);
    selector.set_current(current);
}

}  // namespace tortilla
