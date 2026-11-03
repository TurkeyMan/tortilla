// The log's Show menu Pins and hidden dialog on a repository, for judging
// it without a log window. (Pick... is Browse References, checking
// branches: the app's own window, too tied to it to preview alone.)
//   tortilla-show-dialogs-preview <repository> [--ui=gtk3]
#include "log/branch_filter.h"
#include "log/show_dialogs.h"

#include <rapport/rapport.h>

#include <cstring>

namespace tortilla
{

// The dialogs' windows, without Tortilla's icon (ui/repo_icon.cpp's).
std::unique_ptr<rap::window> create_window(rap::ui& ui, const rap::window_spec& spec,
                                           rap::window* parent)
{
    return ui.create(spec, parent);
}

}  // namespace tortilla

int main(int argc, char** argv)
{
    rap::load_options opts;
    std::string path = ".";
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--ui=", 5) == 0)
            opts.backend = argv[i] + 5;
        else
            path = argv[i];
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui) return 1;
    auto repo = tortilla::repo::open(path);
    if (!repo) return 1;
    const tortilla::ref_map refs = repo->refs();
    auto quit = [&] { (*ui)->quit(); };
    tortilla::pins_hidden_dialog(
        **ui, nullptr, refs,
        {.pins = tortilla::default_pins(*repo), .hidden = {"dependabot/*", "renovate/*"}},
        tortilla::default_pins(*repo), [&](tortilla::pins_and_hidden) { quit(); });
    return (*ui)->run();
}
