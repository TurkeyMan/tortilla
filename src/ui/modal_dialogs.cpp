#include "modal_dialogs.h"

#include <map>

namespace tortilla
{
namespace
{

// UI thread only, as the windows are.
std::map<const rap::window*, size_t>& counts()
{
    static std::map<const rap::window*, size_t> c;
    return c;
}

}  // namespace

std::shared_ptr<const void> count_modal(const rap::window* parent)
{
    if (!parent) return nullptr;
    ++counts()[parent];
    // (The count, not the window: it's only ever compared.)
    return std::shared_ptr<const void>(parent,
                                       [](const void* p)
                                       {
                                           auto& c = counts();
                                           auto it = c.find(static_cast<const rap::window*>(p));
                                           if (it != c.end() && --it->second == 0) c.erase(it);
                                       });
}

size_t modals_over(const rap::window* w)
{
    auto it = counts().find(w);
    return it == counts().end() ? 0 : it->second;
}

void show_message(rap::ui& ui, rap::window* parent, const rap::message_spec& m,
                  std::function<void(rap::dialog_role)> done)
{
    ui.message(parent, m,
               [token = count_modal(parent), done = std::move(done)](rap::dialog_role role) mutable
               {
                   token.reset();
                   if (done) done(role);
               });
}

void pick_files(rap::ui& ui, rap::window* parent, const rap::file_dialog_spec& f,
                std::function<void(std::vector<std::string>)> done)
{
    ui.choose_files(parent, f,
                    [token = count_modal(parent),
                     done = std::move(done)](std::vector<std::string> paths) mutable
                    {
                        token.reset();
                        if (done) done(std::move(paths));
                    });
}

}  // namespace tortilla
