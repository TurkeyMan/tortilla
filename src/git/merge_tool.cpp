#include "merge_tool.h"

#include "git/command.h"
#include "git/remotes.h"
#include "i18n.h"
#include "settings.h"
#include "util/programs.h"

#include <rapport/sys.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{
namespace
{

std::string word(const std::string& program)
{
    return program.find(' ') != std::string::npos ? "\"" + program + "\"" : program;
}

std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string read_file(const fs::path& p)
{
    std::string out;
    if (std::FILE* f = rap::sys::fopen(p, "rb"))
    {
        char buf[4096];
        while (size_t n = std::fread(buf, 1, sizeof buf, f)) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

void replace_all(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

}  // namespace

std::vector<detected_merge_tool> detect_merge_tools()
{
    struct known
    {
        std::string name;
        std::vector<std::string> programs;  // paths to try, then names on PATH
        std::string arguments;
    };
    std::vector<known> list;
    // Command lines from TortoiseGit's manual where it gives one [spec 9.2],
    // else from the tools' own documentation (and git's mergetools).
#ifdef _WIN32
    const std::vector<std::string> roots = install_roots();
    auto at = [&](std::initializer_list<const char*> rel, const char* on_path)
    {
        std::vector<std::string> out;
        for (const std::string& root : roots)
            for (const char* r : rel) out.push_back(root + "\\" + r);
        if (on_path) out.push_back(on_path);
        return out;
    };
    list = {
        {"TortoiseGitMerge", at({"TortoiseGit\\bin\\TortoiseGitMerge.exe"}, nullptr),
         "/base:%base /theirs:%theirs /mine:%mine /merged:%merged /basename:%bname "
         "/theirsname:%tname "
         "/minename:%yname /mergedname:%mname"},
        {"KDiff3", at({"KDiff3\\kdiff3.exe", "KDiff3\\bin\\kdiff3.exe"}, "kdiff3"),
         "%base %mine %theirs -o %merged --L1 %bname --L2 %yname --L3 %tname"},
        {"Meld", at({"Meld\\Meld.exe"}, "meld"), "%mine %base %theirs --output %merged"},
        {"Beyond Compare",
         at({"Beyond Compare 5\\BComp.exe", "Beyond Compare 4\\BComp.exe"}, "BComp"),
         "%mine %theirs %base %merged /lefttitle=%yname /righttitle=%tname /centertitle=%bname "
         "/outputtitle=%mname"},
        {"P4Merge", at({"Perforce\\p4merge.exe"}, "p4merge"), "%base %theirs %mine %merged"},
        {"WinMerge", at({"WinMerge\\WinMergeU.exe"}, "WinMergeU"),
         "/e /ub /fr /wl /wm /dl %bname /dm %tname /dr %yname %base %theirs %mine /o %merged /ar"},
        {"Araxis Merge", at({"Araxis\\Araxis Merge\\Compare.exe"}, nullptr),
         "/wait /3 /title1:%tname /title2:%bname /title3:%yname %theirs %base %mine %merged /a2"},
        {"Sublime Merge", at({"Sublime Merge\\smerge.exe"}, "smerge"),
         "mergetool %base %mine %theirs -o %merged"},
    };
    // JetBrains IDEs merge in the IDE: `<launcher> merge mine theirs base merged`.
    for (const std::string& root : roots)
    {
        std::error_code ec;
        for (const auto& ide :
             fs::directory_iterator(rap::sys::path_from_utf8(root + "\\JetBrains"), ec))
            for (const char* launcher :
                 {"idea64.exe", "clion64.exe", "rider64.exe", "pycharm64.exe", "webstorm64.exe",
                  "goland64.exe", "phpstorm64.exe", "rustrover64.exe"})
                if (fs::is_regular_file(ide.path() / "bin" / launcher, ec))
                    list.push_back({rap::sys::path_to_utf8(ide.path().filename()),
                                    {rap::sys::path_to_utf8(ide.path() / "bin" / launcher)},
                                    "merge %mine %theirs %base %merged"});
    }
#else
    list = {
        {"Meld", {"meld"}, "%mine %base %theirs --output %merged"},
        {"KDiff3",
         {"kdiff3"},
         "%base %mine %theirs -o %merged --L1 %bname --L2 %yname --L3 %tname"},
        {"Beyond Compare",
         {"bcomp"},
         "%mine %theirs %base %merged -lefttitle=%yname -righttitle=%tname"},
        {"P4Merge", {"p4merge"}, "%base %theirs %mine %merged"},
        {"Visual Studio Code", {"code"}, "--wait --merge %theirs %mine %base %merged"},
        {"Sublime Merge", {"smerge"}, "mergetool %base %mine %theirs -o %merged"},
    };
#endif
    std::vector<detected_merge_tool> found;
    for (const known& k : list)
        for (const std::string& program : k.programs)
            if (!find_program(program).empty())
            {
                found.push_back({k.name, word(program) + " " + k.arguments});
                break;
            }
    return found;
}

std::string saved_merge_command()
{
    return settings::get().value("merge.command");
}

void save_merge_command(const std::string& c)
{
    settings::get().set("merge.command", c);
}

std::string merge_command()
{
    std::string c = saved_merge_command();
    if (c.empty())
        if (auto tools = detect_merge_tools(); !tools.empty()) c = tools.front().command;
    return c;
}

std::expected<conflict_sides, std::string> write_conflict_sides(const std::string& workdir,
                                                                const std::string& path)
{
    conflict_sides s{.path = path};
    // The stages there are: 1 base, 2 ours (HEAD), 3 theirs.
    const git_result u =
        run_git(workdir, {"ls-files", "-u", "-z", "--", path}, git_output::stdout_only);
    if (!u.ok()) return std::unexpected(u.output);
    bool stage[4] = {};
    for (size_t at = 0; at < u.output.size();)
    {
        size_t end = u.output.find('\0', at);
        if (end == std::string::npos) end = u.output.size();
        const std::string e = u.output.substr(at, end - at);  // "<mode> <hash> <stage>\t<path>"
        at = end + 1;
        const size_t tab = e.find('\t');
        if (tab == std::string::npos || tab < 2) continue;
        if (e.starts_with("160000")) s.submodule = true;
        const int n = e[tab - 1] - '0';
        if (n >= 1 && n <= 3) stage[n] = true;
    }
    if (!stage[1] && !stage[2] && !stage[3])
        return std::unexpected(rap::i18n::format(_("“{0}” isn't in conflict."), path));

    // During a rebase (tortilla's, or git's) HEAD is the upstream being built
    // on and the pick is the user's own commit: mine is stage 3.
    const fs::path git_dir = rap::sys::path_from_utf8(trimmed(
        run_git(workdir, {"rev-parse", "--absolute-git-dir"}, git_output::stdout_only).output));
    std::error_code ec;
    const fs::path active = git_dir / "tortilla-rebase-active";
    const bool rebasing = fs::exists(active, ec) || fs::exists(git_dir / "rebase-merge", ec) ||
                          fs::exists(git_dir / "rebase-apply", ec);
    const int mine = rebasing ? 3 : 2, theirs = rebasing ? 2 : 3;
    s.mine_deleted = !stage[mine];
    s.theirs_deleted = !stage[theirs];

    // Titles [spec 9.1.6].
    s.base_title = _("Base");
    if (rebasing)
    {
        std::string onto = _("upstream"), branch = _("your branch");
        const std::string text = read_file(active);
        if (size_t at = text.find("onto="); at != std::string::npos)
            onto = text.substr(at + 5, text.find('\n', at) - at - 5);
        if (size_t at = text.find("branch="); at != std::string::npos)
            branch = text.substr(at + 7, text.find('\n', at) - at - 7);
        s.mine_title = branch;
        s.theirs_title = onto;
    }
    else
    {
        const std::string head = current_branch(workdir);
        s.mine_title = head.empty() ? "HEAD" : head;
        auto short_of = [&](const char* ref) { return resolve(workdir, ref).substr(0, 8); };
        if (fs::exists(git_dir / "MERGE_HEAD", ec))
            s.theirs_title = "MERGE_HEAD (" + short_of("MERGE_HEAD") + ")";
        else if (fs::exists(git_dir / "CHERRY_PICK_HEAD", ec))
            s.theirs_title = "CHERRY_PICK_HEAD (" + short_of("CHERRY_PICK_HEAD") + ")";
        else if (fs::exists(git_dir / "REVERT_HEAD", ec))
            s.theirs_title = rap::i18n::format(_("Parent of {0}"), short_of("REVERT_HEAD"));
        else
            s.theirs_title = _("Theirs");
    }
    if (s.mine_deleted || s.theirs_deleted || s.submodule) return s;

    // <file>.BASE<ext> beside the file, each side as checked out (filters
    // and line endings applied) [spec 9.1.3].
    // As the system writes paths (C:\..., not C:/...): tools such as
    // copy take /x for a switch.
    const fs::path file =
        (rap::sys::path_from_utf8(workdir) / rap::sys::path_from_utf8(path)).make_preferred();
    const std::string ext = rap::sys::path_to_utf8(file.extension());
    s.merged = rap::sys::path_to_utf8(file);
    auto side = [&](const char* kind) { return rap::sys::path_to_utf8(file) + "." + kind + ext; };
    s.base = side("BASE");
    s.mine = side("LOCAL");
    s.theirs = side("REMOTE");
    const std::pair<int, std::string*> wanted[] = {
        {1, &s.base}, {mine, &s.mine}, {theirs, &s.theirs}};
    for (auto [n, target] : wanted)
    {
        const fs::path to = rap::sys::path_from_utf8(*target);
        fs::remove(to, ec);
        if (!stage[n])
        {
            // Added on both sides: no base, an empty one.
            if (std::FILE* f = rap::sys::fopen(to, "wb")) std::fclose(f);
            continue;
        }
        // "<temp name>\t<path>": the temp file is relative to the root.
        const git_result t = run_git(
            workdir, {"checkout-index", "--temp", "--stage=" + std::to_string(n), "--", path},
            git_output::stdout_only);
        const size_t tab = t.output.find('\t');
        if (!t.ok() || tab == std::string::npos)
        {
            remove_conflict_sides(s);
            return std::unexpected(rap::i18n::format(_("Could not write {0}: {1}"),
                                                     rap::sys::path_to_utf8(to), t.output));
        }
        fs::rename(rap::sys::path_from_utf8(workdir) /
                       rap::sys::path_from_utf8(t.output.substr(0, tab)),
                   to, ec);
        if (ec)
        {
            remove_conflict_sides(s);
            return std::unexpected(rap::i18n::format(_("Could not write {0}: {1}"),
                                                     rap::sys::path_to_utf8(to), ec.message()));
        }
    }
    return s;
}

void remove_conflict_sides(const conflict_sides& s)
{
    std::error_code ec;
    for (const std::string* p : {&s.base, &s.mine, &s.theirs})
        if (!p->empty()) fs::remove(rap::sys::path_from_utf8(*p), ec);
}

std::vector<std::string> merge_tool_command(const std::string& command, const conflict_sides& s,
                                            const std::string& workdir)
{
    std::string c = command;
    if (c.find("%base") == std::string::npos && c.find("%mine") == std::string::npos &&
        c.find("%theirs") == std::string::npos && c.find("%merged") == std::string::npos)
        c += " %base %theirs %mine %merged";
    // Words: spaces separate them outside quotes; quotes go.
    std::vector<std::string> words;
    std::string w;
    bool in_word = false;
    char quote = 0;
    for (char ch : c)
    {
        if (quote)
        {
            if (ch == quote)
                quote = 0;
            else
                w += ch;
        }
        else if (ch == '"' || ch == '\'')
            quote = ch, in_word = true;
        else if (ch == ' ' || ch == '\t')
        {
            if (in_word) words.push_back(w), w.clear(), in_word = false;
        }
        else
            w += ch, in_word = true;
    }
    if (in_word) words.push_back(w);
    for (std::string& word : words)
    {
        // Titles before files: %bname would otherwise be %base + "name".
        replace_all(word, "%bname", s.base_title);
        replace_all(word, "%yname", s.mine_title);
        replace_all(word, "%tname", s.theirs_title);
        replace_all(word, "%mname", s.path);
        replace_all(word, "%base", s.base);
        replace_all(word, "%mine", s.mine);
        replace_all(word, "%theirs", s.theirs);
        replace_all(word, "%merged", s.merged);
        replace_all(word, "%wtroot",
                    rap::sys::path_to_utf8(rap::sys::path_from_utf8(workdir).make_preferred()));
    }
    return words;
}

bool still_conflicted(const std::string& workdir, const std::string& path)
{
    const git_result u = run_git(workdir, {"ls-files", "-u", "--", path}, git_output::stdout_only);
    return u.ok() && u.output.find_first_not_of(" \r\n") != std::string::npos;
}

}  // namespace tortilla
