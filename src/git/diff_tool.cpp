#include "diff_tool.h"

#include "git/command.h"
#include "settings.h"

#include <rapport/sys.h>

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{

std::string git_diff_tool(const std::string& repo_dir)
{
    git_result r = run_git(repo_dir, {"config", "diff.tool"}, git_output::stdout_only);
    std::string tool = r.ok() ? r.output : "";
    while (!tool.empty() && (tool.back() == '\n' || tool.back() == '\r')) tool.pop_back();
    return tool;
}

diff_mode saved_diff_mode(const std::string& repo_dir)
{
    const std::string mode = settings::get().value("diff.mode");
    if (mode == "builtin") return diff_mode::builtin;
    if (mode == "git") return diff_mode::git;
    if (mode == "custom") return diff_mode::custom;
    return git_diff_tool(repo_dir).empty() ? diff_mode::builtin : diff_mode::git;
}

std::string diff_command()
{
    return settings::get().value("diff.command");
}

void save_diff_settings(diff_mode mode, const std::string& command)
{
    settings& st = settings::get();
    st.set("diff.mode", mode == diff_mode::builtin ? "builtin"
                        : mode == diff_mode::git   ? "git"
                                                  : "custom");
    st.set("diff.command", command);
}

namespace
{

// A shell word for `s` in double quotes (git runs difftool commands with sh,
// Git for Windows' too).
std::string quoted(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\' || c == '$' || c == '`') out += '\\';
        out += c;
    }
    return out + "\"";
}

void replace_all(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

// Where a program would run from: a path, or a name found on PATH.
std::string find_program(const std::string& name)
{
    std::error_code ec;
    const fs::path p = rap::sys::path_from_utf8(name);
    if (p.has_parent_path()) return fs::is_regular_file(p, ec) ? name : "";
#ifdef _WIN32
    const char sep = ';';
    const std::vector<std::string> exts{".exe", ".cmd", ".bat", ""};
#else
    const char sep = ':';
    const std::vector<std::string> exts{""};
#endif
    const std::string path = rap::sys::env("PATH").value_or("");
    for (size_t at = 0; at <= path.size();)
    {
        size_t end = std::min(path.find(sep, at), path.size());
        if (end > at)
            for (const std::string& ext : exts)
            {
                fs::path candidate = rap::sys::path_from_utf8(path.substr(at, end - at)) /
                                     rap::sys::path_from_utf8(name + ext);
                if (fs::is_regular_file(candidate, ec)) return rap::sys::path_to_utf8(candidate);
            }
        at = end + 1;
    }
    return "";
}

}  // namespace

std::string expand_placeholders(const std::string& command, const std::string& base_name,
                               const std::string& mine_name)
{
    std::string s = command;
    // A placeholder the user already quoted keeps their quotes.
    replace_all(s, "\"%base\"", "%base");
    replace_all(s, "\"%mine\"", "%mine");
    replace_all(s, "\"%bname\"", "%bname");
    replace_all(s, "\"%yname\"", "%yname");
    replace_all(s, "%bname", quoted(base_name));
    replace_all(s, "%yname", quoted(mine_name));
    replace_all(s, "%base", "\"$LOCAL\"");
    replace_all(s, "%mine", "\"$REMOTE\"");
    // Without %base or %mine the two sides go last.
    if (command.find("%base") == std::string::npos && command.find("%mine") == std::string::npos)
        s += " \"$LOCAL\" \"$REMOTE\"";
    return s;
}

std::vector<std::string> custom_difftool_args(const std::string& command,
                                            const std::vector<std::string>& revisions,
                                            const std::string& base_name,
                                            const std::string& mine_name,
                                            const std::vector<std::string>& paths)
{
    std::vector<std::string> args{"-c", "difftool.tortilla.cmd=" +
                                            expand_placeholders(command, base_name, mine_name),
                                  "difftool"};
    if (paths.size() != 1) args.push_back("--dir-diff");
    args.insert(args.end(), {"--tool=tortilla", "--no-prompt"});
    args.insert(args.end(), revisions.begin(), revisions.end());
    if (!paths.empty())
    {
        args.push_back("--");
        args.insert(args.end(), paths.begin(), paths.end());
    }
    return args;
}

std::vector<detected_tool> detect_diff_tools()
{
    struct known_tool
    {
        std::string name;
        std::vector<std::string> programs;  // names on PATH, or paths
        std::string arguments;
    };
    std::vector<known_tool> known;
#ifdef _WIN32
    std::vector<std::string> roots;
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"})
        if (auto v = rap::sys::env(var))
            roots.push_back(*v + (std::string(var) == "LOCALAPPDATA" ? "\\Programs" : ""));
    auto under = [&](std::initializer_list<const char*> rel)
    {
        std::vector<std::string> out;
        for (const std::string& root : roots)
            for (const char* r : rel) out.push_back(root + "\\" + r);
        return out;
    };
    // The usual install folders, then PATH (installed elsewhere).
    auto at = [&](std::initializer_list<const char*> rel, const char* on_path)
    {
        std::vector<std::string> out = under(rel);
        out.push_back(on_path);
        return out;
    };
    known = {
        {"WinMerge", at({"WinMerge\\WinMergeU.exe"}, "WinMergeU"),
         "-e -u -dl %bname -dr %yname %base %mine"},
        {"Beyond Compare",
         at({"Beyond Compare 5\\BCompare.exe", "Beyond Compare 4\\BCompare.exe"}, "BCompare"),
         "%base %mine /title1=%bname /title2=%yname"},
        {"Meld", at({"Meld\\Meld.exe"}, "meld"), "%base %mine --label=%bname --label=%yname"},
        {"KDiff3", at({"KDiff3\\kdiff3.exe", "KDiff3\\bin\\kdiff3.exe"}, "kdiff3"),
         "%base %mine --L1 %bname --L2 %yname"},
    };
#else
    known = {
        {"Meld", {"meld"}, "%base %mine --label=%bname --label=%yname"},
        {"KDiff3", {"kdiff3"}, "%base %mine --L1 %bname --L2 %yname"},
        {"Beyond Compare", {"bcompare"}, "%base %mine -title1=%bname -title2=%yname"},
        {"Kompare", {"kompare"}, "%base %mine"},
        {"Diffuse", {"diffuse"}, "%base %mine"},
    };
#endif
    std::vector<detected_tool> found;
    for (const known_tool& k : known)
        for (const std::string& program : k.programs)
            if (!find_program(program).empty())
            {
                // A name found on PATH stays a name; a path is quoted if it
                // has spaces ("C:\Program Files\...").
                const std::string exe =
                    program.find(' ') != std::string::npos ? "\"" + program + "\"" : program;
                found.push_back({k.name, exe + " " + k.arguments});
                break;
            }
    return found;
}

}  // namespace tortilla
