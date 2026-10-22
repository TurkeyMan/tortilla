// Compares the Actions column icon styles: each style's five icons at the
// log's row size, then zoomed x3.
//   tortilla-icon-preview [--ui=gtk3]
#include "log/action_icons.h"

#include <rapport/rapport.h>

#include <cstring>

int main(int argc, char** argv)
{
    rap::LoadOptions opts;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--ui=", 5) == 0) opts.backend = argv[i] + 5;
    auto ui = rap::Ui::load(argc, argv, opts);
    if (!ui) return 1;

    using tortilla::ActionIconStyle;
    static const std::pair<ActionIconStyle, const char*> styles[] = {
        {ActionIconStyle::Glyph, "1. Glyphs (current)"},
        {ActionIconStyle::Document, "2. Classic documents"},
        {ActionIconStyle::Bevel, "3. Bevelled tiles"},
        {ActionIconStyle::Line, "4. Line icons"},
    };
    static const tortilla::Action actions[] = {tortilla::ActionModified, tortilla::ActionAdded,
                                             tortilla::ActionDeleted, tortilla::ActionRenamed,
                                             tortilla::ActionConflicted};

    auto paint = [](rap::Painter& p, rap::SizeF size)
    {
        p.rect({0, 0, size.w, size.h}, 0, rap::Paint{rap::ThemeColor::Base}, std::nullopt);
        float y = 16;
        p.text({200, 0}, "row size", {.bold = true});
        p.text({340, 0}, "zoomed x3", {.bold = true});
        for (auto [style, name] : styles)
        {
            p.text({16, y + 18}, name, {});
            // Actual size: 13 px icons in 16 px slots, on a normal and a
            // selected row.
            for (int sel = 0; sel < 2; ++sel)
            {
                float ry = y + 8 + sel * 18;
                if (sel)
                    p.rect({196, ry, 90, 18}, 0, rap::Paint{rap::ThemeColor::Highlight},
                           std::nullopt);
                for (int n = 0; n < 5; ++n)
                    tortilla::paintActionIcon(p, actions[n], 200 + n * 16 + 1.5f, ry + 2.5f, 13,
                                            style);
            }
            for (int n = 0; n < 5; ++n)
                tortilla::paintActionIcon(p, actions[n], 340 + n * 50, y + 4, 39, style);
            y += 60;
        }
    };

    using namespace rap::layout;
    auto win =
        (*ui)->create(Window{"Actions icon styles",
                             {
                                 Canvas{}.onPaint(paint).width(620_px).height(260_px).stretch(),
                             }});
    win->onCloseRequest(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->show();
    return (*ui)->run();
}
