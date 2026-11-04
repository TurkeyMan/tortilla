#include "show_dialogs.h"

#include "i18n.h"
#include "ui/repo_icon.h"

#include <algorithm>
#include <memory>

namespace tortilla
{
namespace
{

std::string short_name(std::string_view full)
{
    for (std::string_view prefix : {"refs/heads/", "refs/remotes/"})
        if (full.starts_with(prefix)) return std::string(full.substr(prefix.size()));
    return std::string(full);
}

// Every local branch, then every remote one, by full name.
std::vector<std::string> branch_names(const ref_map& refs)
{
    std::vector<std::string> local, remote;
    for (const auto& [id, labels] : refs)
        for (const ref& r : labels)
            if (r.kind == ref_kind::local_branch)
                local.push_back(r.full_name);
            else if (r.kind == ref_kind::remote_branch)
                remote.push_back(r.full_name);
    std::ranges::sort(local);
    std::ranges::sort(remote);
    local.insert(local.end(), remote.begin(), remote.end());
    return local;
}

// A list of words: the pins (by short name), or the hide patterns.
class word_rows : public rap::list_model
{
public:
    std::vector<std::string> words;
    bool pins = false;
    void changed() { notify_reset(); }
    size_t row_count() const override { return words.size(); }
    std::string text(size_t row, size_t) const override
    {
        if (pins && words[row] == "HEAD") return _("HEAD (what's checked out)");
        return pins ? short_name(words[row]) : words[row];
    }
};

}  // namespace

void pins_hidden_dialog(rap::ui& ui, rap::window* parent, const ref_map& refs,
                        pins_and_hidden current, std::vector<std::string> defaults,
                        std::function<void(pins_and_hidden)> done)
{
    struct state
    {
        std::unique_ptr<rap::window> window;
        word_rows pins, hidden;
        std::vector<std::string> branches;  // full names, for the pin box
        rap::list *pin_list = nullptr, *hide_list = nullptr;
        rap::combo* pin = nullptr;
        rap::entry* hide = nullptr;
    };
    auto st = std::make_shared<state>();
    state* s = st.get();
    s->pins.pins = true;
    s->pins.words = std::move(current.pins);
    s->hidden.words = std::move(current.hidden);
    s->branches = branch_names(refs);
    std::vector<std::string> suggestions{"HEAD"};
    for (const std::string& b : s->branches) suggestions.push_back(short_name(b));

    // A branch typed or picked, by its short name or its full one.
    auto add_pin = [s]
    {
        const std::string text = s->pin->text();
        if (text.empty()) return;
        std::string full = text;
        if (text != "HEAD" && !text.starts_with("refs/"))
        {
            auto it = std::ranges::find_if(s->branches, [&](const std::string& b)
                                           { return short_name(b) == text; });
            if (it == s->branches.end()) return;
            full = *it;
        }
        if (std::ranges::find(s->pins.words, full) == s->pins.words.end())
            s->pins.words.push_back(full);
        s->pins.changed();
        s->pin->set_text("");
    };
    auto add_hide = [s]
    {
        std::string text = s->hide->text();
        std::erase(text, ' ');
        if (text.empty()) return;
        if (std::ranges::find(s->hidden.words, text) == s->hidden.words.end())
            s->hidden.words.push_back(text);
        s->hidden.changed();
        s->hide->set_text("");
    };
    auto remove = [](word_rows& rows, rap::list* list)
    {
        std::vector<size_t> sel = list->selection();
        for (size_t k = sel.size(); k-- > 0;)
            if (sel[k] < rows.words.size())
                rows.words.erase(rows.words.begin() + std::ptrdiff_t(sel[k]));
        rows.changed();
    };

    using namespace rap::layout;
    using namespace rap::literals;
    st->window = create_window(
        ui,
        dialog{
            _("Pins and Hidden Branches"),
            {vbox{group{_("Pinned: always shown"),
                        vbox{
                            hbox{
                                list{{{_("Branch"), 36_ch, true}}, &s->pins}
                                    .bind(s->pin_list)
                                    .selection(rap::selection_mode::multi)
                                    .headers(false)
                                    .compact()
                                    .height(7_lines)
                                    .stretch(),
                                vbox{
                                    button{_("_Remove")}.on_click(
                                        [s, remove] { remove(s->pins, s->pin_list); }),
                                    button{_("_Defaults")}.on_click(
                                        [s, defaults]
                                        {
                                            s->pins.words = defaults;
                                            s->pins.changed();
                                        }),
                                    spacer{},
                                },
                            },
                            hbox{
                                combo{suggestions}.bind(s->pin).on_activate(add_pin).stretch(),
                                button{_("_Pin")}.on_click(add_pin),
                            },
                        }},
                  group{_("Hidden: left out of every preset"),
                        vbox{
                            hbox{
                                list{{{_("Pattern"), 36_ch, true}}, &s->hidden}
                                    .bind(s->hide_list)
                                    .selection(rap::selection_mode::multi)
                                    .headers(false)
                                    .compact()
                                    .height(5_lines)
                                    .stretch(),
                                vbox{
                                    button{_("R_emove")}.on_click(
                                        [s, remove] { remove(s->hidden, s->hide_list); }),
                                    spacer{},
                                },
                            },
                            hbox{
                                entry{}
                                    .bind(s->hide)
                                    .placeholder(_("A branch name, or a pattern: dependabot/*"))
                                    .on_activate(add_hide)
                                    .stretch(),
                                button{_("_Hide")}.on_click(add_hide),
                            },
                        }},
                  buttons{ok, cancel}}}}
            .size(64_ch, 30_lines),
        parent);
    st->window->on_finished(
        [&ui, st, done = std::move(done)](rap::dialog_role role)
        {
            pins_and_hidden out{.pins = st->pins.words, .hidden = st->hidden.words};
            ui.post([st] {});  // the window goes after this returns
            if (role == rap::dialog_role::ok) done(std::move(out));
        });
    st->window->show();
}

}  // namespace tortilla
