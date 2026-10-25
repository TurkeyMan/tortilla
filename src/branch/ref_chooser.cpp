#include "ref_chooser.h"

#include "branch/browse_refs.h"

#include "git/command.h"
#include "git/remotes.h"
#include "i18n.h"

#include <algorithm>

namespace tortilla
{
namespace
{

std::vector<std::string> refs(const std::string& workdir, std::vector<std::string> args)
{
    std::vector<std::string> out;
    const git_result r = run_git(workdir, args, git_output::stdout_only);
    if (!r.ok()) return out;
    size_t at = 0;
    while (at < r.output.size())
    {
        size_t end = r.output.find('\n', at);
        if (end == std::string::npos) end = r.output.size();
        std::string line = r.output.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
        at = end + 1;
    }
    return out;
}

bool exists(const std::string& workdir, const std::string& ref)
{
    return run_git(workdir, {"show-ref", "--verify", "--quiet", ref}, git_output::stdout_only).ok();
}

}  // namespace

ref_chooser::ref_chooser(std::string workdir)
    : workdir_(std::move(workdir))
{
}

int ref_chooser::index_of(kind k) const
{
    return int(k) - (options_.head ? 0 : 1);
}

ref_chooser::kind ref_chooser::chosen() const
{
    return kind(kind_->current() + (options_.head ? 0 : 1));
}

rap::node ref_chooser::layout(const options& o)
{
    using namespace rap::layout;
    using namespace rap::literals;
    options_ = o;
    current_ = tortilla::current_branch(workdir_);
    // Branches: local first, then remote-tracking ones as remotes/<r>/<b>
    // (not <r>/HEAD); both in git's (sorted) order [spec 8.1].
    branches_ = refs(workdir_, {"for-each-ref", "--format=%(refname:short)", "refs/heads"});
    for (std::string& r : refs(workdir_, {"for-each-ref", "--format=%(refname)", "refs/remotes"}))
        if (!r.ends_with("/HEAD")) branches_.push_back(r.substr(5));  // "remotes/<r>/<b>"
    if (o.skip_current) std::erase(branches_, current_);
    tags_ = refs(workdir_, {"for-each-ref", "--format=%(refname:short)", "--sort=version:refname",
                            "refs/tags"});

    std::vector<std::string> kinds;
    if (o.head) kinds.push_back("HEAD");
    kinds.insert(kinds.end(), {_("Branch"), _("Tag"), _("Commit")});
    return group{o.caption,
                 hbox{
                     drop_down{kinds, 0}.bind(kind_).on_changed([this](int) { kind_changed(); }),
                     combo{}.bind(value_).width(50_ch).stretch().on_changed(
                         [this](const std::string& t)
                         {
                             if (chosen() == kind::commit) commit_ = t;
                             if (changed_) changed_();
                         }),
                     // Browse References, to pick the branch [spec 8.1].
                     button{"..."}
                         .bind(browse_)
                         .visible(o.ui != nullptr)
                         .tooltip(_("Browse References"))
                         .on_click(
                             [this]
                             {
                                 pick_ref(*options_.ui, options_.window ? options_.window() : nullptr, workdir_,
                                          {.initial = revision()},
                                          [this](const std::string& picked)
                                          {
                                              if (!picked.empty()) select(picked);
                                          });
                             }),
                 }};
}

// One field serves every kind: the branches, the tags, or a commit typed.
void ref_chooser::kind_changed()
{
    switch (chosen())
    {
    case kind::head:
        value_->set_items({});
        value_->set_text("HEAD");
        value_->set_enabled(false);
        break;
    case kind::branch:
        value_->set_items(branches_);
        value_->set_enabled(true);
        value_->set_text(std::ranges::contains(branches_, current_) ? current_
                         : branches_.empty()                        ? ""
                                                                    : branches_.front());
        break;
    case kind::tag:
        value_->set_items(tags_);
        value_->set_enabled(true);
        value_->set_text(tags_.empty() ? "" : tags_.front());
        break;
    case kind::commit:
        value_->set_items({});
        value_->set_enabled(true);
        value_->set_text(commit_);
        break;
    }
    if (browse_) browse_->set_enabled(chosen() == kind::branch);  // only the branch's [spec 8.1]
    if (changed_) changed_();
}

// A ref resolves to its kind: heads/ a branch, remotes/ a remote branch,
// tags/ a tag, anything else a commit [spec 8.1].
void ref_chooser::select(const std::string& initial)
{
    kind k = options_.head ? kind::head : kind::branch;
    std::string value;
    if (!initial.empty())
    {
        const git_result r =
            run_git(workdir_,
                    {"rev-parse", "--symbolic-full-name", "--verify", "--end-of-options", initial},
                    git_output::stdout_only);
        std::string full = r.ok() ? r.output : "";
        while (!full.empty() && (full.back() == '\n' || full.back() == '\r')) full.pop_back();
        if (full.empty()) full = initial;
        if (full.starts_with("refs/")) full.erase(0, 5);
        if (full.starts_with("heads/"))
            k = kind::branch, value = full.substr(6);
        else if (full.starts_with("remotes/"))
            k = kind::branch, value = full;
        else if (full.starts_with("tags/"))
        {
            k = kind::tag, value = full.substr(5);
            if (value.ends_with("^{}")) value.resize(value.size() - 3);
        }
        else
            k = kind::commit, value = initial;
    }
    if (k == kind::branch && !value.empty() && !std::ranges::contains(branches_, value))
        branches_.insert(branches_.begin(), value);
    if (k == kind::commit) commit_ = value;
    kind_->set_current(index_of(k));
    kind_changed();
    if (!value.empty()) value_->set_text(value);
    if (changed_) changed_();
}

std::string ref_chooser::name() const
{
    return chosen() == kind::head ? "HEAD" : value_->text();
}

std::string ref_chooser::revision() const
{
    const std::string n = name();
    const kind k = chosen();
    // A branch and a tag of one name: say which [spec 8.1].
    if ((k == kind::branch || k == kind::tag) && !n.starts_with("remotes/") &&
        exists(workdir_, "refs/heads/" + n) && exists(workdir_, "refs/tags/" + n))
        return (k == kind::branch ? "refs/heads/" : "refs/tags/") + n;
    return n;
}

}  // namespace tortilla
