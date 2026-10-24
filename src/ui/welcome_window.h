// What Tortilla shows when it's started with nothing to open (from the Start
// menu or a launcher), and as its About: the mascot, what Tortilla is and
// why, a way in (open a repository, clone, Settings), who made it, and the
// legal links.
#pragma once

#include <rapport/rapport.h>

#include <functional>
#include <memory>

namespace tortilla
{

class welcome_window
{
public:
    static std::unique_ptr<welcome_window> open(rap::ui&);
    void show() { window_->show(); }
    void on_closed(std::function<void()> f) { closed_ = std::move(f); }

private:
    explicit welcome_window(rap::ui& ui)
        : ui_(ui)
    {
    }
    void build();
    void paint_header(rap::painter&, rap::size_f);
    void open_repository();
    void close();

    rap::ui& ui_;
    std::unique_ptr<rap::window> window_;
    std::unique_ptr<rap::image> mascot_;
    std::function<void()> closed_;
};

}  // namespace tortilla
