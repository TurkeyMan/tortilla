// A page's callbacks, made to do nothing once the page is let go. A page
// taken out of its window (rap::stack::remove) has its widgets destroyed
// only when the toolkit next gets to it, which no number of posts promises:
// until then they hold their callbacks, and those may capture what's gone
// by then. Guarded, they never reach it.
#pragma once

#include <rapport/rapport.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace tortilla
{

class page_guard
{
public:
    // A page built: its callbacks (those guarded from now on) run.
    void open() { token_ = std::make_shared<bool>(true); }
    // Let go: none of them runs again, from this page's widgets or any
    // other's.
    void close() { token_.reset(); }

    // `f`, run only while the page it was made for is open; after, nothing,
    // returning what a default value would (no menu, false, nullopt).
    template <class F>
    auto operator()(F f) const
    {
        return [open = std::weak_ptr<bool>(token_), f = std::move(f)](auto&&... args) mutable
        {
            using result = decltype(f(std::forward<decltype(args)>(args)...));
            if (!open.lock())
            {
                if constexpr (std::is_void_v<result>)
                    return;
                else
                    return result{};
            }
            return f(std::forward<decltype(args)>(args)...);
        };
    }
    // The menu with each action, submenus' too, guarded: a menu kept past
    // the page (by a toolkit, or anyone) does nothing.
    rap::menu menu(rap::menu m) const
    {
        for (rap::menu_item& item : m)
        {
            if (item.action) item.action = (*this)(std::move(item.action));
            item.children = menu(std::move(item.children));
        }
        return m;
    }

private:
    std::shared_ptr<bool> token_;
};

}  // namespace tortilla
