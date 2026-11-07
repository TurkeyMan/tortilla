#include "diff_tool.h"

#include "git/command.h"
#include "git/remotes.h"
#include "git/temp_files.h"
#include "git/tool_discovery.h"
#include "settings.h"
#include "util/programs.h"
#include "util/strings.h"

#include <rapport/sys.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <tuple>

namespace fs = std::filesystem;

namespace tortilla
{

std::string git_diff_tool(const std::string& repo_dir)
{
    return config_value(repo_dir, "diff.tool");
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

bool diff_folders()
{
    return settings::get().value("diff.folders", "1") != "0";
}

void save_diff_settings(diff_mode mode, const std::string& command, bool folders)
{
    settings_changes st;
    st.set("diff.mode", mode == diff_mode::builtin ? "builtin"
                        : mode == diff_mode::git   ? "git"
                                                  : "custom");
    st.set("diff.command", command);
    st.set("diff.folders", folders ? "1" : "0");
    settings::get().apply(st);
}

image_mode saved_image_mode()
{
    const std::string mode = settings::get().value("image.mode");
    if (mode == "text") return image_mode::text;
    if (mode == "ifritview") return image_mode::ifritview;
    if (mode == "custom") return image_mode::custom;
    return find_ifritview().empty() ? image_mode::text : image_mode::ifritview;
}

std::string image_command()
{
    return settings::get().value("image.command");
}

void save_image_settings(image_mode mode, const std::string& command)
{
    settings_changes st;
    st.set("image.mode", mode == image_mode::text        ? "text"
                         : mode == image_mode::ifritview ? "ifritview"
                                                         : "custom");
    st.set("image.command", command);
    settings::get().apply(st);
}

std::string image_diff_command()
{
    switch (saved_image_mode())
    {
    case image_mode::text: return {};
    case image_mode::custom: return image_command();
    case image_mode::ifritview:
    {
        // One window comparing the two, titled with their names; it returns
        // when that closes, so git's temporary files outlive it.
        const std::string exe = find_ifritview();
        return exe.empty() ? std::string() : "\"" + forward_slashes(exe) + "\" --diff %base %mine --labels %bname %yname";
    }
    }
    return {};
}

std::string find_ifritview()
{
#ifdef _WIN32
    // Its installer's App Paths entry, else its InstallDir, else the usual folder.
    auto read = [](const wchar_t* key, const wchar_t* value) -> std::wstring
    {
        wchar_t buf[1024];
        DWORD size = sizeof buf;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
            return {};
        return buf;
    };
    std::wstring exe = read(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\ifritview.exe", nullptr);
    if (exe.empty())
        if (std::wstring dir = read(L"Software\\IfritView", L"InstallDir"); !dir.empty()) exe = dir + L"\\ifritview.exe";
    std::error_code ec;
    if (!exe.empty() && fs::exists(fs::path(exe), ec)) return rap::sys::path_to_utf8(fs::path(exe));
    // An install that wrote neither: where installers put it.
    for (const std::string& root : install_roots())
    {
        const fs::path p = rap::sys::path_from_utf8(root) / "IfritView" / "ifritview.exe";
        if (fs::exists(p, ec)) return rap::sys::path_to_utf8(p);
    }
    return {};
#else
    return find_program("ifritview");
#endif
}

bool is_image_path(const std::string& path)
{
    // What IfritView compares, the formats a repository holds; raw camera
    // files too.
    static const char* const extensions[] = {
        "png", "jpg", "jpeg", "jpe", "jfif", "gif", "bmp", "dib", "tif", "tiff", "webp", "avif",
        "heic", "heif", "jxl", "exr", "hdr", "psd", "tga", "ico", "cur", "qoi", "svg", "dds",
        "ktx", "ktx2", "pnm", "pbm", "pgm", "ppm", "pam", "pcx",
        "cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "pef", "dng",
        "rwl", "3fr", "erf", "kdc", "mef", "mos", "mrw", "x3f", "iiq", "srw", "fff", "dcr"};
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* e : extensions)
        if (ext == e) return true;
    return false;
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

// An empty file in the temporary folder (made once, emptied if something
// wrote to it), with forward slashes; "" if it can't be made.
std::string empty_file()
{
    std::error_code ec;
    const std::filesystem::path dir = tortilla_temp_dir();
    if (dir.empty()) return {};
    const std::filesystem::path file = dir / "empty";
    if (!std::filesystem::exists(file, ec) || std::filesystem::file_size(file, ec) != 0)
        std::ofstream(file, std::ios::binary | std::ios::trunc);
    if (!std::filesystem::exists(file, ec)) return {};
    const std::u8string generic = file.generic_u8string();
    return std::string(generic.begin(), generic.end());
}

std::vector<std::string> custom_difftool_args(const std::string& command,
                                              const std::vector<std::string>& revisions,
                                              const std::string& base_name,
                                              const std::string& mine_name,
                                              const std::vector<std::string>& paths, bool folders)
{
    // A side that doesn't exist (a file's first revision, or its deletion)
    // is /dev/null to git, which most tools can't open (TortoiseGitMerge asks
    // to reload and shows one side): an empty file of ours instead, so the
    // file shows as all added (or removed). Its path is the platform's own
    // (C:/... on Windows): Git for Windows' shell would mangle one like
    // /tmp/x inside an argument such as /base:%base.
    std::string empty_sides;
    if (const std::string empty = empty_file(); !empty.empty())
    {
        std::string quoted = "'";
        for (char c : empty) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        quoted += "'";
        empty_sides = "[ \"$LOCAL\" = /dev/null ] && LOCAL=" + quoted +
                      "; [ \"$REMOTE\" = /dev/null ] && REMOTE=" + quoted + "; ";
    }
    std::vector<std::string> args{"-c",
                                  "difftool.tortilla.cmd=" + empty_sides +
                                      expand_placeholders(command, base_name, mine_name),
                                  "difftool"};
    // A tool that compares only files gets each changed file in turn.
    if (paths.size() != 1 && folders)
    {
        args.push_back("--dir-diff");
#ifdef _WIN32
        // With core.symlinks on, git would link the working tree's files
        // into its temporary folder, which Windows refuses without
        // Developer Mode. Copies work everywhere, and git copies edits back.
        args.push_back("--no-symlinks");
#endif
    }
    args.insert(args.end(), {"--tool=tortilla", "--no-prompt"});
    args.insert(args.end(), revisions.begin(), revisions.end());
    if (!paths.empty())
    {
        args.push_back("--");
        args.insert(args.end(), paths.begin(), paths.end());
    }
    return args;
}

std::vector<detected_tool> image_tools(const std::vector<detected_tool>& diff_tools)
{
    static constexpr std::string_view k_image_capable[] = {"P4Merge", "Beyond Compare", "WinMerge"};
    std::vector<detected_tool> out;
    for (const detected_tool& t : diff_tools)
        if (std::ranges::find(k_image_capable, std::string_view(t.name)) !=
            std::end(k_image_capable))
            out.push_back({t.name, t.command, false});
    return out;
}

std::vector<detected_tool> detect_diff_tools()
{
    // Command lines from each tool's own documentation (and git's mergetools
    // scripts). Each must wait until it's closed: git deletes the temporary
    // copies as soon as it returns.
    struct known_tool
    {
        std::string name;
        std::vector<std::string> programs;  // paths to try, then names on PATH
        std::string arguments;
        bool folders = true;  // false: compares two files only
    };
    std::vector<known_tool> known;
#ifdef _WIN32
    // The usual install folders, then PATH (installed elsewhere).
    known = {
        {"WinMerge", install_candidates({"WinMerge\\WinMergeU.exe"}, "WinMergeU"),
         "/e /u /s- /dl %bname /dr %yname %base %mine"},
        // BComp waits; BCompare hands over to a running copy and returns.
        {"Beyond Compare",
         install_candidates({"Beyond Compare 5\\BComp.exe", "Beyond Compare 4\\BComp.exe"},
                            "BComp"),
         "%base %mine /title1=%bname /title2=%yname"},
        {"Araxis Merge", install_candidates({"Araxis\\Araxis Merge\\Compare.exe"}),
         "/wait /2 /title1:%bname /title2:%yname %base %mine"},
        {"Code Compare", install_candidates({"Devart\\Code Compare\\CodeCompare.exe"}),
         "/W /T1=%bname /T2=%yname %base %mine"},
        {"ExamDiff Pro", install_candidates({"ExamDiff Pro\\ExamDiff.com"}),
         "%base %mine /dn1:%bname /dn2:%yname /nh"},
        {"DeltaWalker",
         install_candidates({"Deltopia\\DeltaWalker\\DeltaWalker.exe",
                             "Deltopia\\DeltaWalker Classic\\DeltaWalker.exe"}),
         "-mi -title1=%bname -title2=%yname %base %mine"},
        {"Meld", install_candidates({"Meld\\Meld.exe"}, "meld"),
         "%base %mine --label=%bname --label=%yname"},
        {"KDiff3", install_candidates({"KDiff3\\kdiff3.exe", "KDiff3\\bin\\kdiff3.exe"}, "kdiff3"),
         "%base %mine --L1 %bname --L2 %yname"},
        {"TortoiseGitMerge", install_candidates({"TortoiseGit\\bin\\TortoiseGitMerge.exe"}),
         "/base:%base /mine:%mine /basename:%bname /minename:%yname", false},
        {"TortoiseMerge", install_candidates({"TortoiseSVN\\bin\\TortoiseMerge.exe"}),
         "/base:%base /mine:%mine /basename:%bname /minename:%yname", false},
        {"P4Merge", install_candidates({"Perforce\\p4merge.exe"}, "p4merge"), "%base %mine", false},
        // bin\code is a shell script, which git's sh runs (code.cmd it can't).
        {"Visual Studio Code", install_candidates({"Microsoft VS Code\\bin\\code"}),
         "--wait --diff %base %mine", false},
    };
    // JetBrains IDEs compare files in the IDE: `<launcher> diff a b`.
    for (const jetbrains_ide& ide : jetbrains_ides())
        known.push_back({ide.name, {ide.launcher}, "diff %base %mine", false});
#else
    known = {
        {"Meld", {"meld"}, "%base %mine --label=%bname --label=%yname"},
        {"KDiff3", {"kdiff3"}, "%base %mine --L1 %bname --L2 %yname"},
        {"Kompare", {"kompare"}, "-c %base %mine"},
        // bcomp waits; bcompare may hand over to a running copy.
        {"Beyond Compare", {"bcomp"}, "%base %mine -title1=%bname -title2=%yname"},
        {"tkdiff", {"tkdiff"}, "-L %bname -L %yname %base %mine"},
        {"xxdiff", {"xxdiff"}, "--title1 %bname --title2 %yname %base %mine"},
        {"DeltaWalker", {"DeltaWalker"}, "-mi -title1=%bname -title2=%yname %base %mine"},
        {"Diffuse", {"diffuse"}, "-L %bname %base -L %yname %mine", false},
        {"P4Merge", {"p4merge"}, "%base %mine", false},
        {"DiffMerge", {"diffmerge"}, "--title1=%bname --title2=%yname %base %mine"},
        {"ECMerge", {"ecmerge"}, "--default --mode=diff2 %base %mine", false},
        {"Guiffy", {"guiffy"}, "%base %mine", false},
        // -f: in the foreground, so the comparison is waited for.
        {"gVim", {"gvim"}, "-f -d %base %mine", false},
        {"Visual Studio Code", {"code"}, "--wait --diff %base %mine", false},
    };
#endif
    std::vector<detected_tool> found;
    auto add = [&](const known_tool& k, const std::string& exe)
    { found.push_back({k.name, exe + " " + k.arguments, k.folders}); };
    for (const known_tool& k : known)
        if (const std::string program = installed_program(k.programs); !program.empty())
            add(k, program);
#ifndef _WIN32
    // A Flatpak app runs through flatpak.
    for (auto [name, app, arguments] :
         {std::tuple{"Meld (Flatpak)", "org.gnome.meld",
                     "%base %mine --label=%bname --label=%yname"},
          std::tuple{"KDiff3 (Flatpak)", "org.kde.kdiff3", "%base %mine --L1 %bname --L2 %yname"},
          std::tuple{"Kompare (Flatpak)", "org.kde.kompare", "-c %base %mine"}})
        if (const std::string run = flatpak_command(app); !run.empty())
            add({name, {}, arguments}, run);
#endif
    return found;
}

}  // namespace tortilla
