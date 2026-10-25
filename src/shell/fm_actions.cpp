#include "shell/fm_actions.h"

#include <rapport/i18n.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

namespace tortilla::fm_actions
{

namespace
{

namespace fs = std::filesystem;
using namespace shell_menu;

// What an entry runs on: every selected path, the first one, or the folder
// (the right-clicked folder or background, else the selected file's).
enum class target
{
    all,
    first,
    folder,
    none,
};

struct action
{
    const char* id;       // menu_table.h's row id, which is also the icon group
    const char* command;  // tortilla's
    target on;
};

constexpr action k_inside[] = {
    {"commit", "commit", target::all},       {"log", "log", target::first},
    {"diff", "diff", target::first},         {"repostatus", "repostatus", target::all},
    {"pull", "pull", target::folder},        {"fetch", "fetch", target::folder},
    {"push", "push", target::folder},        {"switch", "switch", target::folder},
    {"merge", "merge", target::folder},      {"rebase", "rebase", target::folder},
    {"settings", "settings", target::first}, {"about", "about", target::none},
};
constexpr action k_outside[] = {
    {"clone", "clone", target::folder},
    {"settings", "settings", target::first},
    {"about", "about", target::none},
};

const row* row_of(std::string_view id)
{
    for (const row& r : k_rows)
        if (r.label && id == r.id) return &r;
    return nullptr;
}

const action* action_of(std::string_view id)
{
    for (const action& a : k_inside)
        if (id == a.id) return &a;
    for (const action& a : k_outside)
        if (id == a.id) return &a;
    return nullptr;
}

// Without the mnemonic: '&' marks it, "&&" is a literal '&'.
std::string plain(std::string_view s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '&' && (i + 1 == s.size() || s[i + 1] != '&')) continue;
        if (s[i] == '&') ++i;  // "&&"
        out += s[i];
    }
    return out;
}

// Name and Name[de_AT] lines; `prefix` before each ("Git " at the top level).
void write_name_lines(std::ostream& f, const name& n, std::string_view prefix)
{
    f << "Name=" << prefix << n.text << "\n";
    for (const auto& [tag, t] : n.translated)
    {
        std::string key = tag;
        for (char& c : key)
            if (c == '-') c = '_';  // the Desktop Entry spec's locale form
        f << "Name[" << key << "]=" << prefix << t << "\n";
    }
}

// Into place by rename, so a reader sees the old file or the new one.
bool write_file(const fs::path& file, const std::string& text)
{
    const fs::path temp = file.parent_path() / ("." + file.filename().string() + ".tmp");
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!(f << text)) return false;
    }
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) fs::remove(temp, ec);
    return !ec;
}

std::string data_home()
{
    if (const char* d = std::getenv("XDG_DATA_HOME"); d && *d) return d;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.local/share";
}

// The data dir holding the packaged menu, if it's installed.
std::optional<fs::path> packaged_dir()
{
    const char* dirs = std::getenv("XDG_DATA_DIRS");
    const std::string list = dirs && *dirs ? dirs : "/usr/local/share:/usr/share";
    for (size_t at = 0; at <= list.size();)
    {
        size_t colon = list.find(':', at);
        if (colon == std::string::npos) colon = list.size();
        const fs::path d = list.substr(at, colon - at);
        std::error_code ec;
        if (!d.empty() && fs::exists(d / "file-manager/actions/tortilla.desktop", ec)) return d;
        at = colon + 1;
    }
    return std::nullopt;
}

}  // namespace

std::vector<std::string> ids()
{
    std::vector<std::string> out;
    for (const action& a : k_inside) out.push_back(a.id);
    for (const action& a : k_outside)
        if (std::ranges::find(out, a.id) == out.end()) out.push_back(a.id);
    return out;
}

names from_catalogues(const fs::path& translations)
{
    names out;
    for (const std::string& id : ids())
        if (const row* r = row_of(id)) out[id].text = plain(r->label);
    // The label as written in menu_table.h, or failing that without its
    // mnemonic; kept where it differs from the English.
    for (const auto& lang : rap::i18n::available("tortilla", translations))
    {
        rap::i18n::set_languages({lang.tag});
        if (rap::i18n::load("tortilla", translations).empty()) continue;
        for (auto& [id, n] : out)
        {
            const char* label = row_of(id)->label;
            std::string t = rap::i18n::tr("tortilla", label);
            if (t == label) t = rap::i18n::tr("tortilla", n.text.c_str());
            t = plain(t);
            if (t != n.text) n.translated[lang.tag] = t;
        }
    }
    return out;
}

// One a line: id, tab, language tag ("" for the English), tab, name.
void write_names(const fs::path& file, const names& all)
{
    std::ostringstream s;
    for (const auto& [id, n] : all)
    {
        s << id << "\t\t" << n.text << "\n";
        for (const auto& [tag, t] : n.translated) s << id << "\t" << tag << "\t" << t << "\n";
    }
    write_file(file, s.str());
}

names read_names(const fs::path& file)
{
    names out;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);)
    {
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        name& n = out[line.substr(0, a)];
        const std::string tag = line.substr(a + 1, b - a - 1), text = line.substr(b + 1);
        if (tag.empty())
            n.text = text;
        else
            n.translated[tag] = text;
    }
    return out;
}

std::vector<fs::path> write(const fs::path& out, const placements& where, const names& all,
                            const programs& run, bool user)
{
    std::vector<fs::path> written;
    std::error_code ec;
    fs::create_directories(out, ec);
    const std::string prefix = user ? "tortilla-user" : "tortilla";
    const std::string condition = run.condition + (user ? " --user" : "");
    auto name_of = [&](const std::string& id)
    {
        const auto it = all.find(id);
        return it != all.end() ? it->second : name{plain(row_of(id)->label), {}};
    };
    auto put = [&](const std::string& file, const std::string& text)
    {
        if (write_file(out / file, text)) written.push_back(out / file);
    };

    // An action: the entry's command on the right paths. `side`, at the top
    // level, is when it shows (no menu gating it); "" inside a menu.
    auto action_file = [&](const action& a, bool top, std::string_view side)
    {
        std::ostringstream f;
        f << "[Desktop Entry]\nType=Action\n";
        write_name_lines(f, name_of(a.id), top ? "Git " : "");
        f << "Icon=tortilla-menu-" << group_of(a.id) << "\n";
        f << "TargetContext=true\nTargetLocation=true\n";
        const std::string shows =
            side.empty() ? std::string()
                         : "ShowIfTrue=" + condition + " " + std::string(side) + " %f\n";
        const std::string cmd = run.tortilla + " " + a.command;
        if (a.on == target::folder)
        {
            // A folder is its own; a file's is the one it's in.
            f << "Profiles=folder;file;\n\n";
            f << "[X-Action-Profile folder]\nMimeTypes=inode/directory;\nExec=" << cmd << " %f\n"
              << shows << "\n";
            f << "[X-Action-Profile file]\nMimeTypes=all/allfiles;\nExec=" << cmd << " %d\n"
              << shows;
        }
        else
        {
            const char* arg = a.on == target::all ? " %F" : a.on == target::first ? " %f" : "";
            f << "Profiles=main;\n\n[X-Action-Profile main]\nMimeTypes=all/all;\nExec=" << cmd
              << arg << "\n"
              << shows;
        }
        put(prefix + (top ? "-top-" : "-") + a.id + ".desktop", f.str());
    };

    std::set<std::string> in_menu, at_top;
    std::map<std::string, std::set<std::string>> sides;  // a top entry's sides
    std::vector<std::string> inside, outside;            // each submenu's ids
    auto place_all = [&](const auto& entries, std::vector<std::string>& menu, const char* side)
    {
        for (const action& a : entries)
        {
            const shows s = where_shown(*row_of(a.id), where);
            if (s == shows::hidden) continue;
            if (s == shows::top)
            {
                at_top.insert(a.id);
                sides[a.id].insert(side);
            }
            else
            {
                menu.push_back(a.id);
                in_menu.insert(a.id);
            }
        }
    };
    place_all(k_inside, inside, "inside");
    place_all(k_outside, outside, "outside");
    for (const std::string& id : in_menu) action_file(*action_of(id), false, "");
    for (const std::string& id : at_top)
        action_file(*action_of(id), true, sides[id].size() > 1 ? "any" : *sides[id].begin());

    // The two menus, each shown on its side of a working tree; none when
    // every entry is elsewhere.
    auto menu =
        [&](const std::string& file, const char* side, const std::vector<std::string>& entries)
    {
        if (entries.empty()) return;
        std::ostringstream f;
        f << "[Desktop Entry]\nType=Menu\nName=Tortilla\nIcon=tortilla\nItemsList=";
        for (const std::string& id : entries) f << prefix << "-" << id << ";";
        f << "\nTargetContext=true\nTargetLocation=true\n";
        f << "ShowIfTrue=" << condition << " " << side << " %f\n";
        put(file, f.str());
    };
    menu(prefix + ".desktop", "inside", inside);
    menu(prefix + "-outside.desktop", "outside", outside);
    return written;
}

void update_user_set(const placements& where)
{
    const fs::path dir = fs::path(data_home()) / "file-manager/actions";
    const std::optional<fs::path> packaged = packaged_dir();
    // Only the user's set differs from the package's, and only if the
    // choices do for PCManFM's entries.
    bool differs = false;
    for (const std::string& id : ids())
        differs |= where_shown(*row_of(id), where) != where_shown(*row_of(id), placements{});

    std::vector<fs::path> keep;
    if (packaged && differs)
        keep = write(dir, where, read_names(*packaged / "tortilla/fm-names"),
                     {TORTILLA_FM_CONDITION, TORTILLA_FM_PROGRAM}, true);
    // The user's files this set no longer has; nothing else in the folder.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        const std::string n = e.path().filename().string();
        if (n.starts_with("tortilla-user") && n.ends_with(".desktop") &&
            std::ranges::find(keep, e.path()) == keep.end())
            fs::remove(e.path(), ec);
    }
}

}  // namespace tortilla::fm_actions
