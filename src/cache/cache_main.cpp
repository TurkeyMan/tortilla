#include "cache_main.h"

#include "cache/badge_cache.h"
#include "cache/channel.h"

#include <rapport/sys.h>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#endif

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
    if (request.starts_with("badge "))
        return std::to_string(
            int(cache.ask(rap::sys::path_from_utf8(std::string(request.substr(6))))));
    // "<badge> <current> <untracked>", the last two 0 or 1.
    if (request.starts_with("state "))
    {
        const auto s = cache.look(rap::sys::path_from_utf8(std::string(request.substr(6))));
        return std::to_string(int(s.b)) + (s.current ? " 1" : " 0") + (s.untracked ? " 1" : " 0");
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
