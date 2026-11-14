#include "window_identities.h"

#include <utility>

namespace tortilla
{

void window_identities::use(std::shared_ptr<const repo_identity> id)
{
    current_ = std::move(id);
}

std::shared_ptr<const repo_identity> window_identities::created(const void* w, const void* parent)
{
    std::shared_ptr<const repo_identity> id = scope_ ? *scope_ : current_;
    if (parent)
        if (auto it = by_window_.find(parent); it != by_window_.end()) id = it->second;
    // A window at an address that went without word (made elsewhere than
    // create_window) is a new one: replaced.
    by_window_[w] = id;
    return id;
}

void window_identities::destroyed(const void* w)
{
    // Erased first, released after: a lease let go mustn't find the map
    // midway through a change.
    std::shared_ptr<const repo_identity> id;
    if (auto it = by_window_.find(w); it != by_window_.end())
    {
        id = std::move(it->second);
        by_window_.erase(it);
    }
}

std::shared_ptr<const repo_identity> track_window(window_identities& ids, rap::window& w,
                                                  rap::window* parent)
{
    auto id = ids.created(&w, parent);
    // Run once, first thing in w's destructor (at exit too): it touches
    // only the record, by address, none of w's widgets.
    w.on_destroyed([&ids, at = static_cast<const void*>(&w)] { ids.destroyed(at); });
    return id;
}

std::optional<std::shared_ptr<const repo_identity>> window_identities::recorded(const void* w) const
{
    auto it = by_window_.find(w);
    if (it == by_window_.end()) return std::nullopt;
    return it->second;
}

identity_scope::identity_scope(window_identities& ids,
                               std::optional<std::shared_ptr<const repo_identity>> id)
    : ids_(ids)
{
    if (!id) return;
    outer_ = std::exchange(ids_.scope_, std::move(id));
    open_ = true;
}

identity_scope::~identity_scope()
{
    if (open_) ids_.scope_ = std::move(outer_);
}

namespace
{

void scope_actions(window_identities& ids, rap::menu& m,
                   const std::optional<std::shared_ptr<const repo_identity>>& id)
{
    for (rap::menu_item& item : m)
    {
        if (item.action)
            item.action = [&ids, id, f = std::move(item.action)]
            {
                const identity_scope scope(ids, id);
                f();
            };
        scope_actions(ids, item.children, id);
    }
}

}  // namespace

rap::menu in_identity_scope(window_identities& ids,
                            std::optional<std::shared_ptr<const repo_identity>> id, rap::menu m)
{
    if (id) scope_actions(ids, m, id);
    return m;
}

std::function<void()> in_identity_scope(window_identities& ids,
                                        std::optional<std::shared_ptr<const repo_identity>> id,
                                        std::function<void()> f)
{
    if (!id || !f) return f;
    return [&ids, id = std::move(id), f = std::move(f)]
    {
        const identity_scope scope(ids, id);
        f();
    };
}

}  // namespace tortilla
