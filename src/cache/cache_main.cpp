#include "cache_main.h"

#include "cache/badge_cache.h"
#include "cache/badge_changes.h"
#include "cache/channel.h"

#include <rapport/sys.h>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#endif

#include <algorithm>
#include <thread>

namespace tortilla
{

namespace
{

// It ends when nobody has asked for this long; the extension starts it again.
constexpr auto k_idle_exit = std::chrono::minutes(15);

// Tells the file manager these paths' badges changed, so it draws them
// again: Explorer directly, the Linux file managers through their
// extension, which asks what changed (badge_changes.h).
void redraw(const std::vector<std::filesystem::path>& paths, badge_changes& changes)
{
#ifdef _WIN32
    for (const auto& p : paths)
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, p.c_str(), nullptr);
    (void)changes;
#else
    std::vector<std::string> utf8;
    utf8.reserve(paths.size());
    for (const auto& p : paths) utf8.push_back(rap::sys::path_to_utf8(p));
    changes.add(utf8);
#endif
}

}  // namespace

std::string answer_cache_request(badge_cache& cache, std::string_view request,
                                 const badge_changes* changes)
{
    // The paths are slices of the request, looked up as they are: no copies.
    if (request.starts_with("badge ")) return std::to_string(int(cache.look(request.substr(6)).b));
    // Tab-separated paths, each answered "<badge> <current> <untracked>"
    // (one digit each), ';' between them.
    if (request.starts_with("states\t"))
    {
        std::string out;
        out.reserve(size_t(std::ranges::count(request, '\t')) * 6);
        for (std::string_view rest = request.substr(7);;)
        {
            const size_t tab = rest.find('\t');
            const auto s = cache.look(rest.substr(0, tab));
            if (!out.empty()) out += ';';
            out += char('0' + int(s.b));
            out += s.current ? " 1" : " 0";
            out += s.untracked ? " 1" : " 0";
            if (tab == std::string_view::npos) break;
            rest.remove_prefix(tab + 1);
        }
        return out;
    }
    // Not a question about a path: the cache's idle time goes on.
    if (changes && request.starts_with("changes ")) return changes->answer(request.substr(8));
    return "?";
}

int run_cache()
{
    if (!channel::hold_single(k_cache_channel)) return 0;  // one is running
    rap::sys::lower_thread_priority();
    badge_changes changes;
    badge_cache cache([&changes](const std::vector<std::filesystem::path>& paths)
                      { redraw(paths, changes); });
    std::thread(
        [&cache]
        {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::minutes(1));
                if (cache.idle() > k_idle_exit) std::_Exit(0);
            }
        })
        .detach();
    auto served = channel::serve(k_cache_channel, [&](std::string_view request)
                                 { return answer_cache_request(cache, request, &changes); });
    return served ? 0 : 1;
}

}  // namespace tortilla
