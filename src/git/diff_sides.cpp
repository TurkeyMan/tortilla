#include "diff_sides.h"

#include "git/command.h"
#include "git/diff_tool.h"
#include "git/merge_tool.h"
#include "git/temp_files.h"
#include "util/programs.h"

#include <rapport/sys.h>

#include <git2.h>

#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

namespace fs = std::filesystem;

namespace tortilla
{
namespace
{

using repo_ptr = std::unique_ptr<git_repository, decltype(&git_repository_free)>;

repo_ptr open_repo(const std::string& dir)
{
    static const int init = git_libgit2_init();
    (void)init;
    git_repository* r = nullptr;
    if (git_repository_open_ext(&r, dir.empty() ? "." : dir.c_str(), 0, nullptr) != 0) r = nullptr;
    return repo_ptr(r, &git_repository_free);
}

// git's empty tree, which needn't be stored: no tree, to libgit2.
constexpr std::string_view empty_tree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

std::string hex(const git_oid& id)
{
    char s[GIT_OID_MAX_HEXSIZE + 1];
    git_oid_tostr(s, sizeof s, &id);
    return s;
}

bool regular_mode(uint32_t mode)
{
    return mode == 0 || mode == GIT_FILEMODE_BLOB || mode == GIT_FILEMODE_BLOB_EXECUTABLE;
}

// Whether `path` goes through a filter driver of git's config (Git LFS's
// smudge), which libgit2 doesn't run: git writes it then.
bool external_filter(git_repository* repo, const std::string& path)
{
    const char* value = nullptr;
    if (git_attr_get(&value, repo, GIT_ATTR_CHECK_FILE_THEN_INDEX, path.c_str(), "filter") != 0 ||
        git_attr_value(value) != GIT_ATTR_VALUE_STRING)
        return false;
    git_config* config = nullptr;
    if (git_repository_config_snapshot(&config, repo) != 0) return true;  // git knows
    bool driver = false;
    for (const char* key : {".smudge", ".process"})
    {
        git_buf found{};
        const std::string name = std::string("filter.") + value + key;
        driver = driver || git_config_get_string_buf(&found, config, name.c_str()) == 0;
        git_buf_dispose(&found);
    }
    git_config_free(config);
    return driver;
}

// The folder name a revision's files go under: its commit, so HEAD's
// files aren't another HEAD's.
std::string revision_folder(git_repository* repo, const std::string& rev)
{
    git_object* obj = nullptr;
    if (git_revparse_single(&obj, repo, rev.c_str()) != 0) return rev;
    git_object* commit = nullptr;
    const int peeled = git_object_peel(&commit, obj, GIT_OBJECT_COMMIT);
    git_object_free(obj);
    if (peeled != 0) return rev;
    std::string id = hex(*git_object_id(commit));
    git_object_free(commit);
    return id;
}

// The blob `id` as a checkout writes `path`.
std::expected<std::string, std::string> checkout_bytes(git_repository* repo, const std::string& dir,
                                                       const std::string& rev,
                                                       const std::string& path,
                                                       const std::string& id)
{
    if (external_filter(repo, path))
    {
        git_result r = revision_file(dir, rev, path);
        if (!r.ok()) return std::unexpected(r.output);
        return std::move(r.output);
    }
    git_oid oid;
    git_blob* blob = nullptr;
    if (git_oid_fromstr(&oid, id.c_str()) != 0 || git_blob_lookup(&blob, repo, &oid) != 0)
        return std::unexpected(git_error_last() ? git_error_last()->message : "");
    // Every filter applies as git's would: CRLF only to text, which the
    // filter decides itself (an explicit text attribute too).
    git_blob_filter_options o = GIT_BLOB_FILTER_OPTIONS_INIT;
    o.flags = 0;
    git_buf out{};
    const int made = git_blob_filter(&out, blob, path.c_str(), &o);
    std::expected<std::string, std::string> bytes =
        made == 0 ? std::expected<std::string, std::string>(std::string(out.ptr, out.size))
                  : std::unexpected(git_error_last() ? git_error_last()->message : "");
    git_buf_dispose(&out);
    git_blob_free(blob);
    return bytes;
}

std::string platform_path(const fs::path& p)
{
    fs::path copy = p;
    return rap::sys::path_to_utf8(copy.make_preferred());
}

// What sh would make something of: git difftool runs those.
bool needs_shell(std::string_view command)
{
#ifdef _WIN32
    constexpr bool escapes = false;  // C:\Program Files\... is a path here
#else
    constexpr bool escapes = true;
#endif
    char quote = 0;
    for (char c : command)
    {
        if (quote == '\'')
        {
            if (c == '\'') quote = 0;
        }
        else if (quote == '"')
        {
            if (c == '"')
                quote = 0;
            else if (c == '$' || c == '`' || (escapes && c == '\\'))
                return true;
        }
        else if (c == '\'' || c == '"')
            quote = c;
        else if (std::string_view("$`|&;<>()*?[]~#\n\r").find(c) != std::string_view::npos ||
                 (escapes && c == '\\'))
            return true;
    }
    return quote != 0;
}

// `word` with each placeholder replaced, in one pass: a value holding
// "%mine" stays as it is.
std::string
substituted(std::string_view word,
            std::initializer_list<std::pair<std::string_view, const std::string*>> values)
{
    std::string out;
    for (size_t i = 0; i < word.size();)
    {
        bool replaced = false;
        if (word[i] == '%')
            for (const auto& [key, value] : values)
                if (word.substr(i).starts_with(key))
                {
                    out += *value;
                    i += key.size();
                    replaced = true;
                    break;
                }
        if (!replaced) out += word[i++];
    }
    return out;
}

}  // namespace

std::optional<std::vector<file_diff>> diff_files(const std::string& dir,
                                                 const std::vector<std::string>& revisions,
                                                 const std::vector<std::string>& paths)
{
    if (paths.empty() || revisions.empty() || revisions.size() > 2) return std::nullopt;
    repo_ptr repo = open_repo(dir);
    if (!repo) return std::nullopt;
    std::vector<std::unique_ptr<git_tree, decltype(&git_tree_free)>> trees;
    for (const std::string& rev : revisions)
    {
        if (rev == empty_tree)
        {
            trees.emplace_back(nullptr, &git_tree_free);
            continue;
        }
        git_object* obj = nullptr;
        if (git_revparse_single(&obj, repo.get(), rev.c_str()) != 0) return std::nullopt;
        git_object* tree = nullptr;
        const int peeled = git_object_peel(&tree, obj, GIT_OBJECT_TREE);
        git_object_free(obj);
        if (peeled != 0) return std::nullopt;
        trees.emplace_back(reinterpret_cast<git_tree*>(tree), &git_tree_free);
    }
    std::vector<char*> specs;
    for (const std::string& p : paths) specs.push_back(const_cast<char*>(p.c_str()));
    git_diff_options o = GIT_DIFF_OPTIONS_INIT;
    o.pathspec = {specs.data(), specs.size()};
    o.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH | GIT_DIFF_INCLUDE_TYPECHANGE;
    git_diff* raw = nullptr;
    const int made =
        trees.size() == 2
            ? git_diff_tree_to_tree(&raw, repo.get(), trees[0].get(), trees[1].get(), &o)
            : git_diff_tree_to_workdir_with_index(&raw, repo.get(), trees[0].get(), &o);
    if (made != 0) return std::nullopt;
    std::unique_ptr<git_diff, decltype(&git_diff_free)> diff(raw, &git_diff_free);
    // Renames paired, as git diff does by default.
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    find.flags = GIT_DIFF_FIND_RENAMES;
    if (git_diff_find_similar(diff.get(), &find) != 0) return std::nullopt;
    std::vector<file_diff> out;
    for (size_t i = 0, n = git_diff_num_deltas(diff.get()); i < n; ++i)
    {
        const git_diff_delta* d = git_diff_get_delta(diff.get(), i);
        file_diff f;
        f.path = d->new_file.path ? d->new_file.path : d->old_file.path;
        f.old_path = d->old_file.path ? d->old_file.path : f.path;
        switch (d->status)
        {
        case GIT_DELTA_ADDED:
        case GIT_DELTA_UNTRACKED: f.status = 'A'; break;
        case GIT_DELTA_DELETED: f.status = 'D'; break;
        case GIT_DELTA_RENAMED: f.status = 'R'; break;
        case GIT_DELTA_COPIED: f.status = 'C'; break;
        case GIT_DELTA_TYPECHANGE: f.status = 'T'; break;
        default: f.status = 'M'; break;
        }
        if (f.status != 'A') f.old_id = hex(d->old_file.id);
        if (f.status != 'D' && trees.size() == 2) f.new_id = hex(d->new_file.id);
        f.regular =
            f.status != 'T' && regular_mode(d->old_file.mode) && regular_mode(d->new_file.mode);
        out.push_back(std::move(f));
    }
    return out;
}

std::expected<diff_sides, std::string> write_diff_sides(const std::string& dir,
                                                        const std::vector<std::string>& revisions,
                                                        const file_diff& f)
{
    if (revisions.empty() || revisions.size() > 2) return std::unexpected(std::string());
    repo_ptr repo = open_repo(dir);
    if (!repo) return std::unexpected(git_error_last() ? git_error_last()->message : "");
    const std::string empty = empty_file();
    diff_sides sides;
    // A revision's side under the temporary folder; "" where it can't be.
    auto revision_side = [&](const std::string& rev, const std::string& path,
                             const std::string& id) -> std::expected<std::string, std::string>
    {
        std::expected<std::string, std::string> bytes =
            checkout_bytes(repo.get(), dir, rev, path, id);
        if (!bytes) return std::unexpected(bytes.error());
        if (!sides.folder) sides.folder = std::make_shared<const temp_dir>(revision_dir_prefix);
        if (sides.folder->path().empty())
            return std::unexpected(std::string("no temporary folder"));
        const fs::path file =
            revision_temp_path(sides.folder->path(), revision_folder(repo.get(), rev), path);
        if (file.empty() || !write_temp_file(file, *bytes))
            return std::unexpected("Could not write " + rap::sys::path_to_utf8(file));
        sides.written.push_back(file);
        return platform_path(file);
    };
    auto fail = [&](std::string error)
    {
        remove_diff_sides(sides);
        return std::unexpected(std::move(error));
    };
    if (empty.empty()) return std::unexpected(std::string("no temporary folder"));
    const std::string none = platform_path(rap::sys::path_from_utf8(empty));

    if (f.status == 'A')
        sides.base = none;
    else if (auto base = revision_side(revisions[0], f.old_path, f.old_id))
        sides.base = *base;
    else
        return fail(base.error());

    if (f.status == 'D')
        sides.mine = none;
    else if (revisions.size() == 2)
    {
        if (auto mine = revision_side(revisions[1], f.path, f.new_id))
            sides.mine = *mine;
        else
            return fail(mine.error());
    }
    else
    {
        // The working tree's file itself, as git difftool hands it: edits
        // made in the tool are the file's.
        const char* workdir = git_repository_workdir(repo.get());
        if (!workdir) return fail("bare repository");
        sides.mine =
            platform_path(rap::sys::path_from_utf8(workdir) / rap::sys::path_from_utf8(f.path));
    }
    return sides;
}

void remove_diff_sides(diff_sides& sides)
{
    sides.folder.reset();  // the last holder's temp_dir removes it
}

std::optional<std::vector<std::string>>
diff_tool_words(const std::string& command, const std::string& base, const std::string& mine,
                const std::string& base_name, const std::string& mine_name)
{
    if (needs_shell(command)) return std::nullopt;
    std::string c = command;
    if (c.find("%base") == std::string::npos && c.find("%mine") == std::string::npos)
        c += " %base %mine";
    std::vector<std::string> words = command_words(c);
    // A first word NAME=value sets a variable, for sh.
    if (words.empty() || words.front().find('=') != std::string::npos) return std::nullopt;
    std::string program = find_program(words.front());
    if (program.empty()) return std::nullopt;
#ifdef _WIN32
    // CreateProcess runs programs; a .cmd, or a shell script such as VS
    // Code's bin\code, git's sh runs.
    std::string ext = rap::sys::path_to_utf8(rap::sys::path_from_utf8(program).extension());
    for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != ".exe" && ext != ".com") return std::nullopt;
#endif
    words.front() = std::move(program);
    for (auto it = words.begin() + 1; it != words.end(); ++it)
        *it = substituted(
            *it,
            {{"%bname", &base_name}, {"%yname", &mine_name}, {"%base", &base}, {"%mine", &mine}});
    return words;
}

}  // namespace tortilla
