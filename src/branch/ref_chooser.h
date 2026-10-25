// TortoiseGit's version chooser (CChooseVersion) [spec 8.1], shared by the
// Create Branch/Tag, Switch/Checkout and Merge dialogs: a group naming a
// revision as HEAD, a branch (local, or remotes/<remote>/<branch>), a tag
// or a commit. Callers preselect it with any ref or hash (the log's row).
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <string>
#include <vector>

namespace tortilla
{

class ref_chooser
{
public:
    enum class kind
    {
        head,
        branch,
        tag,
        commit,
    };
    struct options
    {
        std::string caption;        // "Base On", "Switch To", "From"
        bool head = false;          // HEAD is a choice (Create)
        bool skip_current = false;  // the current branch isn't (Merge)
        // For the branch's "..." (Browse References as a picker): the ui,
        // and the dialog it opens over.
        rap::ui* ui = nullptr;
        std::function<rap::window*()> window;
    };

    explicit ref_chooser(std::string workdir);

    // The group, for a dialog's layout; widgets bind when it's built.
    rap::node layout(const options&);
    // After the window is made: what's chosen at first. `initial` is any
    // ref or hash; empty chooses the current branch (or HEAD, or the first
    // branch) [spec 8.1].
    void select(const std::string& initial);
    // Called when the choice changes.
    void on_changed(std::function<void()> f) { changed_ = std::move(f); }

    kind chosen() const;
    // The name as shown: a branch, "remotes/<r>/<b>", a tag, or the commit
    // as typed.
    std::string name() const;
    // The revision for git: as name(), with refs/heads/ or refs/tags/ when
    // a branch and a tag share the name; "HEAD" for HEAD.
    std::string revision() const;
    const std::string& current_branch() const { return current_; }

private:
    void kind_changed();
    int index_of(kind) const;

    std::string workdir_;
    options options_;
    std::string current_;
    std::vector<std::string> branches_, tags_;
    std::string commit_;  // the commit field's text, kept while another kind shows
    std::function<void()> changed_;
    rap::drop_down* kind_ = nullptr;
    rap::combo* value_ = nullptr;
    rap::button* browse_ = nullptr;
};

}  // namespace tortilla
