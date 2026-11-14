// The log tab's page: its layout, built into a fresh log_page, and what's
// done once it's built.
#include "log_tab.h"

#include "i18n.h"
#include "settings.h"

#include <algorithm>

namespace tortilla
{
namespace
{

constexpr float k_avatar_least = 32;  // smaller than this, the author's picture isn't drawn

// Held while dragging commits onto another branch, the key that copies
// them (a cherry-pick) rather than moving them: Ctrl, as Explorer, GNOME
// and KDE copy files with it.
constexpr unsigned copy_drag_keys = rap::mod_ctrl;

// TortoiseGit's jump buttons are green arrows (IDI_JUMPUP/IDI_JUMPDOWN).
rap::icon jump_arrow(bool up)
{
    return rap::icon::drawn(
        [up](rap::painter& p, rap::size_f s)
        {
            float w = s.w, h = s.h, cx = w / 2;
            float tip = up ? h * 0.1f : h * 0.9f, base = up ? h * 0.55f : h * 0.45f;
            float tail = up ? h * 0.9f : h * 0.1f;
            rap::color green{0x2e, 0xa0, 0x43}, edge{0x1d, 0x6b, 0x2b};
            p.polygon({{cx, tip},
                       {w * 0.9f, base},
                       {w * 0.65f, base},
                       {w * 0.65f, tail},
                       {w * 0.35f, tail},
                       {w * 0.35f, base},
                       {w * 0.1f, base}},
                      rap::paint{green}, rap::paint{edge});
        });
}

}  // namespace

rap::node log_tab::build_page()
{
    using namespace rap::layout;
    namespace a = rap;

    page_ = std::make_unique<log_page>();
    return vbox{
        // Filter bar [spec 1.2, row 1]
        hbox{
            // Sync, far left: its icon's badge says what the remotes
            // have that the repository hasn't, or that a remote failed.
            button{_("Fetch")}
                .icon(fetch_icon())
                .icon_only()
                .bind(page_->fetch)
                .on_click([this] { sync(); }),
            menu_button{"HEAD"}
                .bind(page_->ref_button)
                .on_menu([this] { return show_menu(); })
                .width(22_ch),
            // What this window did to the refs, undone and redone.
            tool_button{undo_icon(false), _("Undo")}
                .bind(page_->undo)
                .on_click([this] { step_journal(false); }),
            tool_button{undo_icon(true), _("Redo")}
                .bind(page_->redo)
                .on_click([this] { step_journal(true); }),
            entry{search_text_}
                .bind(page_->search)
                .clearable()
                // Emptied (its clear button), at once.
                .on_changed(
                    [this](const std::string& text)
                    {
                        search_text_ = text;
                        if (text.empty() && !filter_.text.empty())
                            apply_filter();
                        else
                            schedule_filter();
                    })
                .on_activate([this] { apply_filter(); })
                // The dates [spec 1.9.3], rarely limited: a pop-up
                // of their own from the box's end, the button held
                // down while they are.
                .trailing_action(
                    calendar_icon(), _("Date range"), [this] { open_dates(); }, page_->dates)
                // Which fields the text is looked for in, and how:
                // TortoiseGit's magnifier at the box's start.
                .leading_menu(rap::icon::named("edit-find-symbolic"), _("Filter options"),
                              [this] { return filter_menu(); })
                .stretch(),
            drop_down{{_("Author Email"), _("Committer Email"), _("Merge Point"), _("Parent 1"),
                       _("Parent 2"), _("Tag"), _("Tag (FF)"), _("Branch"), _("Branch (FF)"),
                       _("Selection History")},
                      jump_type_}
                .bind(page_->jump_type)
                .on_changed([this](int type) { jump_type_ = type; }),
            button{_("Jump up (Alt+Up)")}
                .icon(jump_arrow(true))
                .icon_only()
                .on_click([this] { jump(true); }),
            button{_("Jump down (Alt+Down)")}
                .icon(jump_arrow(false))
                .icon_only()
                .on_click([this] { jump(false); }),
        },
        // The current branch behind its upstream, after a sync.
        hbox{
            label{}.bind(page_->hint_text).ellipsize().stretch(),
            button{_("_Fast-forward")}
                .bind(page_->hint_ff)
                .on_click([this] { fast_forward_current(); }),
            button{_("_Pull...")}.bind(page_->hint_pull).on_click([this] { keep_pull_dialog(); }),
            button{_("Hide")}.flat().on_click(
                [this]
                {
                    hint_hidden_ = current_.branch + ' ' + current_.upstream + ' ' +
                                   std::to_string(current_.behind);
                    show_branch_hint(current_);
                }),
        }
            .handle(page_->hint_bar)
            .visible(false),
        // Commit list, details, changed files [spec 1.2, main area]
        splitter{
            vertical,
            list{log_model::columns(), &log_}
                .soft_selection()
                .auto_scroll()
                .bind(page_->commit_list)
                .on_selection_changed([this] { selection_changed(); })
                .on_context_menu([this](std::optional<size_t> row) { return context_menu(row); })
                .on_header_menu([this](size_t) { return header_menu(); })
                .on_cell_drag(label_drag())
                // Commits dragged within the log: reordered on their
                // branch, or moved or copied onto another one
                // (history_actions.cpp).
                .on_reorder_or_copy([this](const std::vector<size_t>& rows, size_t destination,
                                           rap::drop_action action)
                                    { return reorder_rows(rows, destination, action); },
                                    copy_drag_keys)
                // Commits dragged out, into a rebase's list or anywhere
                // taking text: their hashes, newest first as the rows run.
                .on_drag_out(
                    {.begin = [this](std::vector<size_t> rows) -> std::optional<rap::data_offer>
                     {
                         std::ranges::sort(rows);
                         std::vector<std::string> hashes;
                         for (size_t row : rows)
                             if (const commit* c = log_.commit(row)) hashes.push_back(c->id.hex());
                         if (hashes.empty()) return std::nullopt;
                         const std::string data = commits_payload(hashes);
                         return rap::data_offer{{{commits_type, data}, {"text/plain", data}}};
                     }})
                .on_activate([this](size_t row) { row_activated(row); })
                .on_cell_context_menu([this](size_t row, size_t column, rap::point_f at)
                                      { return cell_menu(row, column, at); })
                .stretch(3),
            // The forge panel, a column beside the commit's text and
            // files, when the commit has a PR or CI to show.
            splitter{
                horizontal,
                // The details pane keeps its height: dragging the upper
                // divider takes space from the file list until that
                // reaches its minimum. (TortoiseGit shrinks the details
                // first [spec 1.3]; this is deliberate.)
                splitter{
                    vertical,
                    hbox{
                        text_view{}.read_only().bind(page_->details).stretch(),
                        // Kept top right, as TortoiseGit has it [spec 1.2].
                        vbox{
                            canvas{}
                                .on_paint(
                                    [this](rap::painter& p, rap::size_f size)
                                    {
                                        p.rect({0, 0, size.w, size.h}, 0,
                                               rap::paint{rap::theme_color::window}, std::nullopt);
                                        if (!avatar_) return;
                                        // Laid out smaller than asked (a
                                        // short details pane): whole and
                                        // in shape at its top right, or
                                        // not at all once too small to
                                        // make out.
                                        const float iw = float(avatar_->width()),
                                                    ih = float(avatar_->height());
                                        const rap::rect_f at =
                                            rap::fit_inside({0, 0, size.w, size.h}, iw, ih, 1, 0);
                                        if (std::min(at.w, at.h) < k_avatar_least) return;
                                        p.image(*avatar_, {0, 0, iw, ih}, at);
                                    })
                                .width(rap::extent{k_avatar_size, rap::extent::unit::px})
                                .height(rap::extent{k_avatar_size, rap::extent::unit::px})
                                .bind(page_->avatar_canvas),
                            spacer{},
                        },
                    }
                        .height(9_lines),
                    files_.list("log.files").height(4_lines).stretch(2),
                }
                    .bind(page_->details_splitter)
                    .width(30_ch)  // the least the forge panel's bar leaves it
                    .stretch(3),
                forge_panel_->layout(),
            }
                .bind(page_->forge_splitter)
                .stretch(2),
        }
            .bind(page_->splitter)
            .stretch(),
        // Bottom area [spec 1.2]
        hbox{
            // A sync's red "!", while its warning is on show.
            canvas{}
                .focusable(false)
                .bind(page_->status_mark)
                .width(rap::extent{18, rap::extent::unit::px})
                .visible(false)
                .on_paint([](rap::painter& p, rap::size_f s) { paint_failure_mark(p, s); })
                // Clicked: what git said.
                .on_pointer(
                    [this](const rap::pointer_event& e)
                    {
                        if (e.type != rap::pointer_event_type::press || e.button != 1) return false;
                        show_sync_output();
                        return true;
                    }),
            label{}.selectable().ellipsize().bind(page_->status).stretch(),
        },
        hbox{
            vbox{
                // TortoiseGit's Show Whole Project the other way up, to
                // say what's in effect: ticked, the history of the
                // path only. Per repository, saved as TortoiseGit's
                // [spec 1.9.7]; only with a path to leave out.
                check{rap::i18n::format(_("_Only changes to {0}"), path_text(40)), only_path_}
                    .bind(page_->only_path)
                    .visible(!rel_path_.empty())
                    .tooltip(path_text(0))
                    .on_toggled(
                        [this](bool only)
                        {
                            only_path_ = only;
                            settings::get().set("log.wholeProject." + repo_.workdir(),
                                                only ? "0" : "1");
                            if (host_) host_->tab_changed(*this);
                            update_file_view();  // nothing is unrelated without a path
                            reload();
                        }),
            },
            // The selected commit's files, by path: a second after
            // typing stops, at once on Enter, cleared by Escape (the
            // window's keys) [spec 1.11.2].
            entry{path_filter_text_}
                .bind(page_->path_filter)
                .placeholder(std::string("   ") + _("Filter paths"))
                .on_changed(
                    [this](const std::string& text)
                    {
                        path_filter_text_ = text;
                        if (!page_) return;
                        page_->path_filter_timer =
                            ui_.timer(1000, false, [this] { update_file_view(); });
                    })
                .on_activate([this] { update_file_view(); })
                .stretch(),
        },
        hbox{
            // A placeholder until TortoiseGit's statistics dialog is built.
            button{_("S_tatistics")}
                .bind(page_->statistics)
                .enabled(false)
                .tooltip(_("Not yet in Tortilla")),
            progress_bar{}.bind(page_->progress).stretch(),
            menu_button{_("Walk Be_havior")}
                .bind(page_->walk)
                .on_menu([this] { return walk_menu(); }),
            menu_button{_("_View")}.bind(page_->view).on_menu([this] { return view_menu(); }),
            spacer{},
            button{_("OK")}.bind(page_->ok).on_click(
                [this]
                {
                    if (picked_)
                    {
                        std::vector<commit> chosen;
                        for (size_t row : page_->commit_list->selection())
                            if (const commit* c = log_.commit(row); c && c->parents.size() <= 1)
                                chosen.push_back(*c);
                        picked_(std::move(chosen));
                    }
                    if (host_) host_->finish(*this, true);
                }),
            button{_("Cancel")}
                .bind(page_->cancel)
                .on_click(
                    [this]
                    {
                        if (host_) host_->finish(*this, false);
                    }),
        },
    };
}

void log_tab::bind_page(std::optional<log_view_state> view)
{
    if (!page_) return;
    // Clicked, the status bar's "!" says what git said.
    page_->status_mark->set_cursor(rap::cursor::pointer);
    page_->forge_splitter->on_moved(
        [this]
        {
            if (!page_) return;
            // The share it's dragged to is the one it keeps.
            rap::window* window = parent();
            if (forge_panel_visible_ && window)
            {
                forge_sizes_ = page_->forge_splitter->sizes();
                float total = 0;
                for (float s : forge_sizes_) total += s;
                forge_share_.fitted(total, window->size().w);
            }
            fit_files();
        });
    forge_panel_->built();
    if (view)
        apply_view(*view);
    else
        apply_view_settings();
    refresh_page();
    // Built again: the commit's text and files shown, once there are rows.
    if (view && log_.loading())
        show_selected_ = true;
    else if (view)
        selection_changed();
    page_->fit_timer = ui_.timer(300, false, [this] { fit_files(); });
}

log_view_state log_tab::capture_view()
{
    log_view_state v;
    if (!page_) return v;
    flush_filters();
    // Still to be put back (a load under way), or as it is.
    v.selection = kept_ ? kept_ : keep_selection();
    v.panes = page_->splitter->sizes();
    v.details_panes = page_->details_splitter->sizes();
    v.column_widths = page_->commit_list->column_widths();
    v.column_order = page_->commit_list->column_order();
    for (size_t i = 0; i < log_model::column_count; ++i)
        v.column_visible.push_back(page_->commit_list->column_visible(i));
    v.files = files_.view_state();
    for (size_t row : files_.selection())
        if (row < files_.files().size()) v.selected_files.push_back(files_.files()[row].path);
    using focus = log_view_state::focus;
    v.focused = page_->commit_list->has_focus()   ? focus::commits
                : page_->search->has_focus()      ? focus::search
                : page_->path_filter->has_focus() ? focus::path_filter
                : files_.has_focus()              ? focus::files
                                                  : focus::none;
    return v;
}

// Sizes of 0 (a window not laid out yet) say nothing: left as built.
void log_tab::apply_view(const log_view_state& v)
{
    auto sized = [](const std::vector<float>& sizes)
    {
        float total = 0;
        for (float s : sizes) total += s;
        return sizes.size() == 2 && total > 0;
    };
    if (sized(v.panes)) page_->splitter->set_sizes(v.panes);
    if (sized(v.details_panes)) page_->details_splitter->set_sizes(v.details_panes);
    if (!v.column_widths.empty()) page_->commit_list->set_column_widths(v.column_widths);
    if (!v.column_order.empty()) page_->commit_list->set_column_order(v.column_order);
    for (size_t i = 0; i < v.column_visible.size(); ++i)
        page_->commit_list->set_column_visible(i, v.column_visible[i]);
    files_.apply_view_state(v.files);
    // The files come back once the selection is shown again (show_commit).
    if (!v.selected_files.empty()) files_.select_paths_when_set(v.selected_files);
    if (v.selection) kept_ = v.selection;
    using focus = log_view_state::focus;
    switch (v.focused)
    {
    case focus::commits: page_->commit_list->focus(); break;
    case focus::search: page_->search->focus(); break;
    case focus::path_filter: page_->path_filter->focus(); break;
    case focus::files: files_.focus(); break;
    case focus::none: break;
    }
}

void log_tab::flush_filters()
{
    if (search_text_ != filter_.text) apply_filter();
    if (page_ && page_->path_filter_timer) update_file_view();
}

void log_tab::drop_page()
{
    if (!page_) return;
    flush_filters();
    // The forge column's divider, kept as it would be on hiding it.
    if (forge_panel_visible_)
    {
        const auto sizes = page_->forge_splitter->sizes();
        float total = 0;
        for (float s : sizes) total += s;
        if (total > 0) forge_sizes_ = sizes;
    }
    forge_panel_->unbind();
    files_.unbind();
    show_selected_ = false;
    page_.reset();
}

// F5 reloads [TortoiseGit: Refresh], e.g. after git ran elsewhere.
bool log_tab::key(const rap::key_event& e)
{
    if (!e.press || !page_) return false;
    // Undo and Redo in the commit list (text fields keep theirs:
    // their keys don't come here).
    if ((e.modifiers & ~rap::mod_shift) == rap::mod_ctrl && page_->commit_list->has_focus() &&
        e.key == rap::key::character && (e.codepoint == 'z' || e.codepoint == 'y'))
    {
        step_journal(e.codepoint == 'y' || (e.modifiers & rap::mod_shift));
        return true;
    }
    if (e.modifiers) return false;
    // Escape in the filter box clears it, at once.
    if (e.key == rap::key::escape && page_->search->has_focus() && !search_text_.empty())
    {
        search_text_.clear();
        page_->search->set_text("");
        if (!filter_.text.empty()) apply_filter();  // unless hearing it did
        return true;
    }
    // Escape in the path filter box clears it [spec 1.11.2].
    if (e.key == rap::key::escape && page_->path_filter->has_focus() && !path_filter_text_.empty())
    {
        path_filter_text_.clear();
        page_->path_filter->set_text("");
        update_file_view();
        return true;
    }
    if (e.key != rap::key::f5) return false;
    reload();
    return true;
}

}  // namespace tortilla
