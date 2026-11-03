#include "busy_pointer.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace tortilla
{

std::function<void()> show_busy_pointer(rap::ui& ui, int ms)
{
    struct state
    {
        bool ended = false;
        rap::subscription timer;
    };
    // Kept here until they end, which the timer makes sure of.
    static std::vector<std::shared_ptr<state>> showing;
    static int count = 0;
    if (count++ == 0) ui.set_busy(true);
    auto s = std::make_shared<state>();
    showing.push_back(s);
    auto end = [&ui, weak = std::weak_ptr<state>(s)]
    {
        const std::shared_ptr<state> s = weak.lock();
        if (!s || s->ended) return;
        s->ended = true;
        if (--count == 0) ui.set_busy(false);
        // Let go after this call, which may be the timer's own.
        ui.post([s] { std::erase(showing, s); });
    };
    s->timer = ui.timer(ms, false, end);
    return end;
}

}  // namespace tortilla
