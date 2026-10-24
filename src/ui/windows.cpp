#include "windows.h"

#include <map>

namespace tortilla::windows
{
namespace
{

rap::ui* g_ui = nullptr;
std::map<size_t, std::shared_ptr<void>> g_kept;
size_t g_next = 1;

}  // namespace

void init(rap::ui& ui)
{
    g_ui = &ui;
}

size_t hold(std::shared_ptr<void> owner)
{
    g_kept.emplace(g_next, std::move(owner));
    return g_next++;
}

void release(size_t id)
{
    if (!g_ui) return;
    g_ui->post(
        [id]
        {
            g_kept.erase(id);
            if (g_kept.empty()) g_ui->quit();
        });
}

}  // namespace tortilla::windows
