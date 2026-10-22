#include "diff_window.h"

#include "git/command.h"
#include "settings.h"

#include <rapport/sys.h>

#include <cctype>
#include <cstdio>
#include <memory>

namespace tortilla
{

std::vector<rap::TextView::Span> diffSpans(const std::string& text)
{
    std::vector<rap::TextView::Span> spans;
    bool inHeader = false;
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + at, end - at);

        rap::TextStyle s;
        if (line.starts_with("diff ") || line.starts_with("commit "))
        {
            inHeader = true;
            s.bold = true;
        }
        else if (line.starts_with("@@"))
        {
            inHeader = false;
            s.fg = rap::ThemeColor::Accent;
        }
        else if (inHeader)
        {
            if (line.starts_with("+++") || line.starts_with("---")) s.bold = true;
        }
        else if (line.starts_with('+'))
            s.fg = rap::ThemeColor::Positive;
        else if (line.starts_with('-'))
            s.fg = rap::ThemeColor::Negative;

        if (s.fg || s.bold) spans.push_back({at, end, s});
        at = end + 1;
    }
    return spans;
}

std::vector<size_t> findAll(const std::string& text, const std::string& needle, bool matchCase)
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
    const std::string hay = matchCase ? text : fold(text), pin = matchCase ? needle : fold(needle);
    for (size_t at = hay.find(pin); at != std::string::npos; at = hay.find(pin, at + 1))
        out.push_back(at);
    return out;
}

namespace
{

// The window and its find bar; owns itself until closed.
class DiffWindow : public std::enable_shared_from_this<DiffWindow>
{
public:
    DiffWindow(rap::Ui& ui, std::string title, std::function<void()> onClosed)
        : ui_(ui)
        , title_(std::move(title))
        , onClosed_(std::move(onClosed))
    {
    }

    void open(const std::string& dir, const std::vector<std::string>& gitArgs)
    {
        using namespace rap::layout;
        auto self = shared_from_this();
        std::weak_ptr<DiffWindow> weak = self;
        auto with = [weak](auto f)
        {
            return [weak, f]
            {
                if (auto s = weak.lock()) f(*s);
            };
        };
        window_ = ui_.create(Window{
            title_ + " - Tortilla",
            {VBox{
                TextView{}.readOnly().monospace().wrap(false).bind(text_).stretch(),
                HBox{
                    Label{"Find:"},
                    Entry{}
                        .bind(query_)
                        .onChanged(
                            [weak](const std::string&)
                            {
                                if (auto s = weak.lock()) s->search();
                            })
                        .onActivate(with([](DiffWindow& w) { w.step(1); }))
                        .stretch(),
                    Button{"_Previous"}.onClick(with([](DiffWindow& w) { w.step(-1); })),
                    Button{"_Next"}.onClick(with([](DiffWindow& w) { w.step(1); })),
                    Check{"Match _case"}
                        .bind(matchCase_)
                        .onToggled(
                            [weak](bool)
                            {
                                if (auto s = weak.lock()) s->search();
                            }),
                    Label{""}.bind(count_).width(12_ch),
                }
                    .margins()  // the window has none, so the text sits flush as in an editor
                    .handle(findBar_),
            }}}
                                 .menuBar({
                                     // The menus live in the window, so their items hold the
                                     // DiffWindow weakly, like every other callback here.
                                     {"_File",
                                      [with]
                                      {
                                          return rap::Menu{
                                              {.label = "_Save As...",
                                               .action = with([](DiffWindow& w) { w.saveAs(); }),
                                               .shortcut = "Ctrl+S"},
                                              rap::MenuItem::separator(),
                                              {.label = "_Close",
                                               .action = with([](DiffWindow& w) { w.close(); }),
                                               .shortcut = "Ctrl+W"},
                                          };
                                      }},
                                     {"_Edit",
                                      [with, weak]
                                      {
                                          auto s = weak.lock();
                                          const bool any = s && !s->query_->text().empty();
                                          return rap::Menu{
                                              {.label = "_Find...",
                                               .action = with([](DiffWindow& w) { w.showFind(); }),
                                               .shortcut = "Ctrl+F"},
                                              {.label = "Find _Next",
                                               .action = with([](DiffWindow& w) { w.step(1); }),
                                               .shortcut = "F3",
                                               .enabled = any},
                                              {.label = "Find _Previous",
                                               .action = with([](DiffWindow& w) { w.step(-1); }),
                                               .shortcut = "Shift+F3",
                                               .enabled = any},
                                          };
                                      }},
                                 })
                                 .margins(false)
                                 .size(120_ch, 45_lines));

        findBar_->setVisible(false);
        window_->onKey(
            [weak](const rap::KeyEvent& e)
            {
                auto s = weak.lock();
                return s && s->key(e);
            });
        window_->onCloseRequest(
            [weak]
            {
                if (auto s = weak.lock()) s->closed();
                return true;
            });

        GitResult r = runGit(dir, gitArgs, GitOutput::Stdout);
        content_ = r.output;
        text_->setText(content_,
                       r.ok() ? diffSpans(content_) : std::vector<rap::TextView::Span>{});
        self_ = self;  // alive until closed
        Settings::get().restoreWindow("diff.window", *window_);
        window_->show();
    }

private:
    bool key(const rap::KeyEvent& e)
    {
        if (!e.press) return false;
        const bool ctrl = e.modifiers & rap::ModCtrl, shift = e.modifiers & rap::ModShift;
        using rap::Key;
        if (e.key == Key::F3)
        {
            step(shift ? -1 : 1);
            return true;
        }
        if (e.key == Key::Escape && findShown_)
        {
            findShown_ = false;
            findBar_->setVisible(false);
            text_->focus();
            return true;
        }
        if (ctrl && e.key == Key::Character) switch (e.codepoint)
            {
            case 'f': showFind(); return true;
            case 's': saveAs(); return true;
            case 'w': close(); return true;
            }
        return false;
    }

    void showFind()
    {
        // A selection seeds the search, as in most editors.
        auto [a, b] = text_->selection();
        if (b > a && b - a < 200 && content_.find('\n', a) >= b)
            query_->setText(content_.substr(a, b - a));
        findShown_ = true;
        findBar_->setVisible(true);
        query_->focus();
        search();
    }

    // Re-runs the search for the query and selects the first match at or
    // after the caret: find as you type.
    void search()
    {
        matches_ = findAll(content_, query_->text(), matchCase_->checked());
        current_ = -1;
        if (matches_.empty())
        {
            count_->setText(query_->text().empty() ? "" : "Not found");
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
            showFind();
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
        count_->setText(std::to_string(current_ + 1) + " of " + std::to_string(matches_.size()));
    }

    void saveAs()
    {
        rap::FileDialogSpec spec;
        spec.mode = rap::FileDialogSpec::Mode::Save;
        spec.title = "Save Diff";
        std::string name;
        for (char c : title_)
            name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ? c : '_';
        spec.initialPath = name + ".patch";
        spec.filters = {{"Patches (*.patch, *.diff)", "*.patch;*.diff"}, {"All files", "*"}};
        std::weak_ptr<DiffWindow> weak = shared_from_this();
        ui_.chooseFiles(window_.get(), spec,
                        [weak](std::vector<std::string> paths)
                        {
                            auto s = weak.lock();
                            if (!s || paths.empty()) return;
                            // GTK's save dialog doesn't add the type's
                            // extension the way Windows' does.
                            std::string path = paths.front();
                            if (!rap::sys::pathFromUtf8(path).has_extension()) path += ".patch";
                            s->write(path);
                        });
    }

    void write(const std::string& path)
    {
        // Binary: the patch keeps git's own line endings.
        std::FILE* f = rap::sys::fopen(rap::sys::pathFromUtf8(path), "wb");
        bool ok = f && std::fwrite(content_.data(), 1, content_.size(), f) == content_.size();
        if (f) ok = std::fclose(f) == 0 && ok;
        if (!ok)
            ui_.message(window_.get(), {.icon = rap::MessageSpec::Icon::Error,
                                        .title = "Save Diff",
                                        .text = "Could not save " + path + "."});
    }

    void close()
    {
        window_->close();
        closed();
    }

    // Destroyed after the callback that closed it returns.
    void closed()
    {
        Settings::get().saveWindow("diff.window", *window_);
        if (onClosed_) onClosed_();
        auto keep = std::move(self_);
        ui_.post([keep] {});
    }

    rap::Ui& ui_;
    std::string title_, content_;
    std::function<void()> onClosed_;
    std::unique_ptr<rap::Window> window_;
    std::shared_ptr<DiffWindow> self_;
    rap::TextView* text_ = nullptr;
    rap::Entry* query_ = nullptr;
    rap::Check* matchCase_ = nullptr;
    rap::Label* count_ = nullptr;
    rap::Widget* findBar_ = nullptr;
    bool findShown_ = false;
    std::vector<size_t> matches_;
    int current_ = -1;
};

}  // namespace

void showUnifiedDiff(rap::Ui& ui, const std::string& title, const std::string& dir,
                     const std::vector<std::string>& gitArgs, std::function<void()> closed)
{
    std::make_shared<DiffWindow>(ui, title, std::move(closed))->open(dir, gitArgs);
}

}  // namespace tortilla
