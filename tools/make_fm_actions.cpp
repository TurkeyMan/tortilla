// Writes PCManFM's and PCManFM-Qt's Tortilla menu: freedesktop "file manager
// actions" (.desktop files in share/file-manager/actions), which libfm and
// libfm-qt read, those file managers having no menu extensions to load.
// The labels are the shell menu's (src/shell/menu_table.h) and their
// translations Tortilla's catalogues, so neither is copied by hand.
//   tortilla-fm-actions OUTDIR TRANSLATIONS_DIR CONDITION TORTILLA
// CONDITION is the installed fm-condition script, TORTILLA the program.
#include "shell/menu_table.h"

#include <rapport/i18n.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using namespace tortilla::shell_menu;

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

// A fixed menu for each side of a working tree: libfm's conditions can't
// see the item's state, as the shell extensions do.
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

const char* label_of(std::string_view id)
{
    for (const row& r : k_rows)
        if (r.label && id == r.id) return r.label;
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

// The label in each language there's a catalogue for, where it differs:
// as written in menu_table.h, or failing that without its mnemonic.
std::map<std::string, std::map<std::string, std::string>>
translations(const fs::path& folder, const std::vector<std::string>& ids)
{
    std::map<std::string, std::map<std::string, std::string>> out;  // id -> tag -> name
    for (const auto& lang : rap::i18n::available("tortilla", folder))
    {
        rap::i18n::set_languages({lang.tag});
        if (rap::i18n::load("tortilla", folder).empty()) continue;
        for (const std::string& id : ids)
        {
            const char* label = label_of(id);
            const std::string bare = plain(label);
            std::string t = rap::i18n::tr("tortilla", label);
            if (t == label) t = rap::i18n::tr("tortilla", bare.c_str());
            t = plain(t);
            if (t != bare) out[id][lang.tag] = t;
        }
    }
    return out;
}

void write_names(std::ofstream& f, const std::string& name,
                 const std::map<std::string, std::string>& translated)
{
    f << "Name=" << name << "\n";
    for (const auto& [tag, t] : translated)
    {
        std::string key = tag;
        for (char& c : key)
            if (c == '-') c = '_';  // the Desktop Entry spec's locale form, de_AT
        f << "Name[" << key << "]=" << t << "\n";
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        std::fprintf(stderr,
                     "usage: tortilla-fm-actions OUTDIR TRANSLATIONS_DIR CONDITION TORTILLA\n");
        return 2;
    }
    const fs::path out = argv[1], folder = argv[2];
    const std::string condition = argv[3], tortilla = argv[4];
    fs::create_directories(out);

    std::vector<std::string> ids;
    for (const action& a : k_inside) ids.push_back(a.id);
    for (const action& a : k_outside) ids.push_back(a.id);
    for (const std::string& id : ids)
        if (!label_of(id))
        {
            std::fprintf(stderr, "tortilla-fm-actions: no menu row \"%s\"\n", id.c_str());
            return 1;
        }
    const auto names = translations(folder, ids);
    auto names_of = [&](const std::string& id)
    {
        auto it = names.find(id);
        return it == names.end() ? std::map<std::string, std::string>{} : it->second;
    };

    std::map<std::string, const action*> actions;
    for (const action& a : k_inside) actions[a.id] = &a;
    for (const action& a : k_outside) actions[a.id] = &a;
    for (const auto& [id, a] : actions)
    {
        std::ofstream f(out / ("tortilla-" + id + ".desktop"));
        f << "[Desktop Entry]\nType=Action\n";
        write_names(f, plain(label_of(id)), names_of(id));
        f << "Icon=tortilla-menu-" << group_of(id) << "\n";
        f << "TargetContext=true\nTargetLocation=true\n";
        const std::string run = tortilla + " " + a->command;
        if (a->on == target::folder)
        {
            // A folder is its own; a file's is the one it's in.
            f << "Profiles=folder;file;\n\n";
            f << "[X-Action-Profile folder]\nMimeTypes=inode/directory;\nExec=" << run << " %f\n\n";
            f << "[X-Action-Profile file]\nMimeTypes=all/allfiles;\nExec=" << run << " %d\n";
        }
        else
        {
            const char* arg = a->on == target::all ? " %F" : a->on == target::first ? " %f" : "";
            f << "Profiles=main;\n\n[X-Action-Profile main]\nMimeTypes=all/all;\nExec=" << run
              << arg << "\n";
        }
    }

    // The two menus, each shown on its side of a working tree.
    auto menu = [&](const char* file, const char* side, auto& entries)
    {
        std::ofstream f(out / file);
        f << "[Desktop Entry]\nType=Menu\nName=Tortilla\nIcon=tortilla\nItemsList=";
        for (const action& a : entries) f << "tortilla-" << a.id << ";";
        f << "\nTargetContext=true\nTargetLocation=true\n";
        f << "ShowIfTrue=" << condition << " " << side << " %f\n";
    };
    menu("tortilla.desktop", "inside", k_inside);
    menu("tortilla-outside.desktop", "outside", k_outside);
    return 0;
}
