#include "diff_tool.h"

#include "git/command.h"
#include "settings.h"

#include <rapport/sys.h>

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{

std::string gitDiffTool(const std::string& repoDir)
{
    GitResult r = runGit(repoDir, {"config", "diff.tool"}, GitOutput::Stdout);
    std::string tool = r.ok() ? r.output : "";
    while (!tool.empty() && (tool.back() == '\n' || tool.back() == '\r')) tool.pop_back();
    return tool;
}

DiffMode diffMode(const std::string& repoDir)
{
    const std::string mode = Settings::get().value("diff.mode");
    if (mode == "builtin") return DiffMode::Builtin;
    if (mode == "git") return DiffMode::Git;
    if (mode == "custom") return DiffMode::Custom;
    return gitDiffTool(repoDir).empty() ? DiffMode::Builtin : DiffMode::Git;
}

std::string diffCommand()
{
    return Settings::get().value("diff.command");
}

void saveDiffSettings(DiffMode mode, const std::string& command)
{
    Settings& st = Settings::get();
    st.set("diff.mode", mode == DiffMode::Builtin ? "builtin"
                        : mode == DiffMode::Git   ? "git"
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

void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

// Where a program would run from: a path, or a name found on PATH.
std::string findProgram(const std::string& name)
{
    std::error_code ec;
    const fs::path p = rap::sys::pathFromUtf8(name);
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
                fs::path candidate = rap::sys::pathFromUtf8(path.substr(at, end - at)) /
                                     rap::sys::pathFromUtf8(name + ext);
                if (fs::is_regular_file(candidate, ec)) return rap::sys::pathToUtf8(candidate);
            }
        at = end + 1;
    }
    return "";
}

}  // namespace

std::string expandPlaceholders(const std::string& command, const std::string& baseName,
                               const std::string& mineName)
{
    std::string s = command;
    // A placeholder the user already quoted keeps their quotes.
    replaceAll(s, "\"%base\"", "%base");
    replaceAll(s, "\"%mine\"", "%mine");
    replaceAll(s, "\"%bname\"", "%bname");
    replaceAll(s, "\"%yname\"", "%yname");
    replaceAll(s, "%bname", quoted(baseName));
    replaceAll(s, "%yname", quoted(mineName));
    replaceAll(s, "%base", "\"$LOCAL\"");
    replaceAll(s, "%mine", "\"$REMOTE\"");
    // Without %base or %mine the two sides go last.
    if (command.find("%base") == std::string::npos && command.find("%mine") == std::string::npos)
        s += " \"$LOCAL\" \"$REMOTE\"";
    return s;
}

std::vector<std::string> customDifftoolArgs(const std::string& command,
                                            const std::vector<std::string>& revisions,
                                            const std::string& baseName,
                                            const std::string& mineName,
                                            const std::vector<std::string>& paths)
{
    std::vector<std::string> args{"-c", "difftool.tortilla.cmd=" +
                                            expandPlaceholders(command, baseName, mineName),
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

std::vector<DetectedTool> detectDiffTools()
{
    struct Known
    {
        std::string name;
        std::vector<std::string> programs;  // names on PATH, or paths
        std::string arguments;
    };
    std::vector<Known> known;
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
    auto at = [&](std::initializer_list<const char*> rel, const char* onPath)
    {
        std::vector<std::string> out = under(rel);
        out.push_back(onPath);
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
    std::vector<DetectedTool> found;
    for (const Known& k : known)
        for (const std::string& program : k.programs)
            if (!findProgram(program).empty())
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
