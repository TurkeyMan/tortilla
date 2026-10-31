#include "cache_main.h"

#include "cache/badge_cache.h"
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
// again. Linux file managers hear it from their extension instead (to come).
void redraw(const std::vector<std::filesystem::path>& paths)
{
#ifdef _WIN32
    for (const auto& p : paths)
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, p.c_str(), nullptr);
#else
    (void)paths;
#endif
}

}  // namespace

std::string answer_cache_request(badge_cache& cache, std::string_view request)
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
    return "?";
}

int run_cache()
{
    if (!channel::hold_single(k_cache_channel)) return 0;  // one is running
    rap::sys::lower_thread_priority();
    badge_cache cache(redraw);
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
    auto served = channel::serve(k_cache_channel, [&cache](std::string_view request)
                                 { return answer_cache_request(cache, request); });
    return served ? 0 : 1;
}

}  // namespace tortilla
