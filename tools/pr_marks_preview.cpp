// The log's pull request marks on a branch label, one of each, drawn as the
// log draws them: for judging the set.
//   tortilla-pr-marks-preview [--ui=gtk3]
#include "log/ref_labels.h"

#include <rapport/rapport.h>

#include <cstring>

namespace
{

using tortilla::check_state;

struct row
{
    tortilla::pr_mark mark;
    const char* says;
};

const row rows[] = {
    {{870, check_state::success}, "CI passed; can be merged"},
    {{869, check_state::success, false, true}, "CI passed; the rules stop it yet (a review)"},
    {{868, check_state::success, false, false, true}, "CI passed; conflicts"},
    {{867, check_state::pending}, "CI running"},
    {{866, check_state::failure}, "CI failed"},
    {{865, check_state::none, false, true}, "no CI; the rules stop it yet"},
    {{805, check_state::none, false, false, true}, "no CI; conflicts"},
    {{804, check_state::none}, "no CI; can be merged"},
    {{803, check_state::success, true}, "a draft (its number fainter)"},
};

class marks : public rap::list_model
{
public:
    size_t row_count() const override { return std::size(rows); }
    std::string text(size_t row, size_t column) const override
    {
        return column == 1 ? rows[row].says : "";
    }
    void paint(size_t row, size_t, rap::painter& p, const rap::cell_info& cell) const override
    {
        const std::vector<tortilla::ref> refs{{.name = "github/ow/tx-floor",
                                               .full_name = "refs/remotes/github/ow/tx-floor",
                                               .kind = tortilla::ref_kind::remote_branch}};
        tortilla::paint_ref_labels(p, refs, 0, cell.size.h, {}, nullptr,
                                   [row](const tortilla::ref&) { return rows[row].mark; });
    }
};

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--ui=", 5) == 0) opts.backend = argv[i] + 5;
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui) return 1;
    marks model;
    using namespace rap::layout;
    using namespace rap::literals;
    auto win = (*ui)->create(window{
        "PR marks",
        {
            list{std::vector<rap::column>{{.title = "Label", .width = 30_ch, .painted = true},
                                          {.title = "Means", .width = 44_ch}},
                 &model}
                .stretch(),
        }}.size(620_px, 260_px));
    win->on_close_request(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->show();
    return (*ui)->run();
}
