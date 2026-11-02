// The log's labels, one of each kind, then its pull request marks on a
// branch label, drawn as the log draws them: for judging the set.
//   tortilla-pr-marks-preview [--ui=gtk3]
#include "log/ref_labels.h"

#include <rapport/rapport.h>

#include <cstring>

namespace
{

using tortilla::check_state;
using tortilla::ref;
using tortilla::ref_kind;

struct row
{
    std::vector<ref> refs;
    std::optional<tortilla::pr_mark> mark;
    const char* says;
    bool pinned = false;
};

ref branch(const char* name, ref_kind kind)
{
    const std::string prefix = kind == ref_kind::local_branch    ? "refs/heads/"
                               : kind == ref_kind::remote_branch ? "refs/remotes/"
                                                                 : "refs/tags/";
    return {.name = name, .full_name = prefix + name, .kind = kind};
}

std::vector<row> make_rows()
{
    ref current = branch("feature", ref_kind::local_branch);
    current.current = true;
    ref worktree = branch("hotfix", ref_kind::local_branch);
    worktree.worktree = "/home/me/src/project-hotfix";
    ref remote_default = branch("origin/main", ref_kind::remote_branch);
    remote_default.remote_default = true;
    ref tracking = branch("main", ref_kind::local_branch);
    tracking.upstream = "refs/remotes/origin/main";
    ref annotated = branch("v1.2", ref_kind::tag);
    annotated.annotated = true;
    const ref remote = branch("github/ow/tx-floor", ref_kind::remote_branch);

    std::vector<row> rows{
        {{branch("topic", ref_kind::local_branch)}, {}, "local branch"},
        {{current}, {}, "current branch"},
        {{branch("origin/topic", ref_kind::remote_branch)}, {}, "remote branch"},
        {{remote_default}, {}, "a remote's default branch"},
        {{worktree}, {}, "checked out in another worktree"},
        {{branch("v1.1", ref_kind::tag)}, {}, "tag"},
        {{annotated}, {}, "annotated tag"},
        {{tracking, remote_default}, {}, "tracking its remote's default, both pinned", true},
        {{branch("topic", ref_kind::local_branch)}, {}, "a pinned local branch", true},
        {{worktree}, {}, "a pinned worktree branch", true},
        {{remote},
         tortilla::pr_mark{870, check_state::success},
         "pinned, with a pull request",
         true},
    };
    const std::pair<tortilla::pr_mark, const char*> marks[] = {
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
    for (const auto& [mark, says] : marks) rows.push_back({{remote}, mark, says});
    return rows;
}

class marks : public rap::list_model
{
public:
    size_t row_count() const override { return rows_.size(); }
    std::string text(size_t row, size_t column) const override
    {
        return column == 1 ? rows_[row].says : "";
    }
    void paint(size_t row, size_t, rap::painter& p, const rap::cell_info& cell) const override
    {
        const struct row& r = rows_[row];
        tortilla::paint_ref_labels(
            p, r.refs, 0, cell.size.h, {}, nullptr, [&](const ref&) { return r.mark; },
            [&](const ref&) { return r.pinned; });
    }

private:
    std::vector<row> rows_ = make_rows();
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
        "Labels and PR marks",
        {
            list{std::vector<rap::column>{{.title = "Label", .width = 32_ch, .painted = true},
                                          {.title = "Means", .width = 44_ch}},
                 &model}
                .stretch(),
        }}.size(660_px, 560_px));
    win->on_close_request(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->show();
    return (*ui)->run();
}
