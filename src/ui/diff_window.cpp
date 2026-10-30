#include "diff_window.h"

#include "git/command.h"
#include "i18n.h"
#include "settings.h"

#include <rapport/sys.h>

#include <cctype>
#include <cstdio>
#include <memory>

namespace tortilla
{

std::vector<rap::text_view::span> diff_spans(const std::string& text)
{
    std::vector<rap::text_view::span> spans;
    bool in_header = false;
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + at, end - at);

        rap::text_style s;
        // "From <hash>" opens each of format-patch's mails: its message,
        // and the "---" before the file list, aren't diff lines.
        if (line.starts_with("diff ") || line.starts_with("commit ") || line.starts_with("From "))
        {
            in_header = true;
            s.bold = true;
        }
        else if (line.starts_with("@@"))
        {
            in_header = false;
            s.fg = rap::theme_color::accent;
        }
        else if (in_header)
        {
            if (line.starts_with("+++") || line.starts_with("---")) s.bold = true;
        }
        else if (line.starts_with('+'))
            s.fg = rap::theme_color::positive;
        else if (line == "-- ")
            in_header = true;  // a format-patch mail's signature: git's version
        else if (line.starts_with('-'))
            s.fg = rap::theme_color::negative;

        if (s.fg || s.bold) spans.push_back({at, end, s});
        at = end + 1;
    }
    return spans;
}

std::vector<size_t> find_all(const std::string& text, const std::string& needle, bool match_case)
{
    std::vector<size_t> out;
    if (needle.empty()) return out;
    // ASCII case folding: diffs are code, and folding bytes keeps offsets
    // aligned with the text.
    auto fold = [](std::string s)
    {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string hay = match_case ? text : fold(text), pin = match_case ? needle : fold(needle);
    for (size_t at = hay.find(pin); at != std::string::npos; at = hay.find(pin, at + 1))
        out.push_back(at);
    return out;
}

namespace
{

// The window and its find bar; owns itself until closed.
class diff_window : public std::enable_shared_from_this<diff_window>
{
public:
    diff_window(rap::ui& ui, std::string title, std::function<void()> on_closed,
                std::function<void(rap::window*)> save)
        : ui_(ui)
        , title_(std::move(title))
        , on_closed_(std::move(on_closed))
        , save_(std::move(save))
    {
    }

    void open(const std::string& dir, const std::vector<std::string>& git_args)
    {
        using namespace rap::layout;
        auto self = shared_from_this();
        std::weak_ptr<diff_window> weak = self;
        auto with = [weak](auto f)
        {
            return [weak, f]
            {
                if (auto s = weak.lock()) f(*s);
            };
        };
        window_ = ui_.create(window{
            rap::i18n::format(_("{0} - Tortilla"), title_),
            {vbox{
                text_view{}.read_only().monospace().wrap(false).bind(text_).stretch(),
                hbox{
                    label{_("Find:")},
                    entry{}
                        .bind(query_)
                        .on_changed(
                            [weak](const std::string&)
                            {
                                if (auto s = weak.lock()) s->search();
                            })
                        .on_activate(with([](diff_window& w) { w.step(1); }))
                        .stretch(),
                    button{_("_Previous")}.on_click(with([](diff_window& w) { w.step(-1); })),
                    button{_("_Next")}.on_click(with([](diff_window& w) { w.step(1); })),
                    check{_("Match _case")}
                        .bind(match_case_)
                        .on_toggled(
                            [weak](bool)
                            {
                                if (auto s = weak.lock()) s->search();
                            }),
                    label{""}.bind(count_).width(12_ch),
                }
                    .margins()  // the window has none, so the text sits flush as in an editor
                    .handle(find_bar_),
            }}}
                                 .menu_bar({
                                     // The menus live in the window, so their items hold the
                                     // diff_window weakly, like every other callback here.
                                     {_("_File"),
                                      [with]
                                      {
                                          return rap::menu{
                                              {.label = _("_Save As..."),
                                               .action = with([](diff_window& w) { w.save_as(); }),
                                               .shortcut = "Ctrl+S"},
                                              rap::menu_item::separator(),
                                              {.label = _("_Close"),
                                               .action = with([](diff_window& w) { w.close(); }),
                                               .shortcut = "Ctrl+W"},
                                          };
                                      }},
                                     {_("_Edit"),
                                      [with, weak]
                                      {
                                          auto s = weak.lock();
                                          const bool any = s && !s->query_->text().empty();
                                          return rap::menu{
                                              {.label = _("_Find..."),
                                               .action = with([](diff_window& w) { w.show_find(); }),
                                               .shortcut = "Ctrl+F"},
                                              {.label = _("Find _Next"),
                                               .action = with([](diff_window& w) { w.step(1); }),
                                               .shortcut = "F3",
                                               .enabled = any},
                                              {.label = _("Find _Previous"),
                                               .action = with([](diff_window& w) { w.step(-1); }),
                                               .shortcut = "Shift+F3",
                                               .enabled = any},
                                          };
                                      }},
                                 })
                                 .margins(false)
                                 .size(120_ch, 45_lines));

        find_bar_->set_visible(false);
        window_->on_key(
            [weak](const rap::key_event& e)
            {
                auto s = weak.lock();
                return s && s->key(e);
            });
        window_->on_close_request(
            [weak]
            {
                if (auto s = weak.lock()) s->closed();
                return true;
            });

        git_result r = run_git(dir, git_args, git_output::stdout_only);
        content_ = r.output;
        // git diff --no-index (two files) exits 1 when they differ.
        const bool diff = r.ok() || r.exit_code == 1;
        text_->set_text(content_,
                        diff ? diff_spans(content_) : std::vector<rap::text_view::span>{});
        self_ = self;  // alive until closed
        settings::get().restore_window("diff.window", *window_);
        window_->show();
    }

private:
    bool key(const rap::key_event& e)
    {
        if (!e.press) return false;
        const bool ctrl = e.modifiers & rap::mod_ctrl, shift = e.modifiers & rap::mod_shift;
        using rap::key;
        if (e.key == key::f3)
        {
            step(shift ? -1 : 1);
            return true;
        }
        if (e.key == key::escape && find_shown_)
        {
            find_shown_ = false;
            find_bar_->set_visible(false);
            text_->focus();
            return true;
        }
        if (ctrl && e.key == key::character) switch (e.codepoint)
            {
            case 'f': show_find(); return true;
            case 's': save_as(); return true;
            case 'w': close(); return true;
            }
        return false;
    }

    void show_find()
    {
        // A selection seeds the search, as in most editors.
        auto [a, b] = text_->selection();
        if (b > a && b - a < 200 && content_.find('\n', a) >= b)
            query_->set_text(content_.substr(a, b - a));
        find_shown_ = true;
        find_bar_->set_visible(true);
        query_->focus();
        search();
    }

    // Re-runs the search for the query and selects the first match at or
    // after the caret: find as you type.
    void search()
    {
        matches_ = find_all(content_, query_->text(), match_case_->checked());
        current_ = -1;
        if (matches_.empty())
        {
            count_->set_text(query_->text().empty() ? "" : _("Not found"));
            return;
        }
        const size_t caret = text_->selection().first;
        current_ = 0;
        while (size_t(current_) < matches_.size() && matches_[size_t(current_)] < caret) ++current_;
        if (size_t(current_) == matches_.size()) current_ = 0;  // wrap
        show();
    }

    void step(int by)
    {
        if (query_->text().empty())
        {
            show_find();
            return;
        }
        if (matches_.empty()) search();
        if (matches_.empty()) return;
        const int n = int(matches_.size());
        current_ = current_ < 0 ? (by > 0 ? 0 : n - 1) : ((current_ + by) % n + n) % n;
        show();
    }

    void show()
    {
        const size_t at = matches_[size_t(current_)];
        text_->select(at, at + query_->text().size());
        count_->set_text(rap::i18n::format(_("{0} of {1}"), current_ + 1, matches_.size()));
    }

    void save_as()
    {
        if (save_) return save_(window_.get());
        rap::file_dialog_spec spec;
        spec.mode = rap::file_dialog_spec::mode::save;
        spec.title = _("Save Diff");
        std::string name;
        for (char c : title_)
            name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ? c : '_';
        spec.initial_path = name + ".patch";
        spec.filters = {{_("Patches (*.patch, *.diff)"), "*.patch;*.diff"}, {_("All files"), "*"}};
        std::weak_ptr<diff_window> weak = shared_from_this();
        ui_.choose_files(window_.get(), spec,
                        [weak](std::vector<std::string> paths)
                        {
                            auto s = weak.lock();
                            if (!s || paths.empty()) return;
                            // GTK's save dialog doesn't add the type's
                            // extension the way Windows' does.
                            std::string path = paths.front();
                            if (!rap::sys::path_from_utf8(path).has_extension()) path += ".patch";
                            s->write(path);
                        });
    }

    void write(const std::string& path)
    {
        // Binary: the patch keeps git's own line endings.
        std::FILE* f = rap::sys::fopen(rap::sys::path_from_utf8(path), "wb");
        bool ok = f && std::fwrite(content_.data(), 1, content_.size(), f) == content_.size();
        if (f) ok = std::fclose(f) == 0 && ok;
        if (!ok)
            ui_.message(window_.get(), {.icon = rap::message_spec::icon::error,
                                        .title = _("Save Diff"),
                                        .text = rap::i18n::format(_("Could not save {0}."), path)});
    }

    void close()
    {
        window_->close();
        closed();
    }

    // Destroyed after the callback that closed it returns.
    void closed()
    {
        settings::get().save_window("diff.window", *window_);
        if (on_closed_) on_closed_();
        auto keep = std::move(self_);
        ui_.post([keep] {});
    }

    rap::ui& ui_;
    std::string title_, content_;
    std::function<void()> on_closed_;
    std::function<void(rap::window*)> save_;
    std::unique_ptr<rap::window> window_;
    std::shared_ptr<diff_window> self_;
    rap::text_view* text_ = nullptr;
    rap::entry* query_ = nullptr;
    rap::check* match_case_ = nullptr;
    rap::label* count_ = nullptr;
    rap::widget* find_bar_ = nullptr;
    bool find_shown_ = false;
    std::vector<size_t> matches_;
    int current_ = -1;
};

}  // namespace

void show_unified_diff(rap::ui& ui, const std::string& title, const std::string& dir,
                       const std::vector<std::string>& git_args, std::function<void()> closed,
                       std::function<void(rap::window*)> save)
{
    std::make_shared<diff_window>(ui, title, std::move(closed), std::move(save))
        ->open(dir, git_args);
}

}  // namespace tortilla
