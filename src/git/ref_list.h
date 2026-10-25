// Every ref with the data Browse References shows for it [spec 10.3], the
// folders they make, and its filter [spec 10.6].
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tortilla
{

struct ref_entry
{
    std::string full;   // refs/heads/main
    std::string hash;   // the object's (a tag object's for annotated tags)
    std::string type;   // commit, tag, tree, blob
    std::string upstream;  // origin/main, "(gone: origin/main)", or ""
    std::string subject, author, committer, description;
    int64_t author_date = 0, commit_date = 0;  // 0: none (a ref to a tree)

    bool commit() const { return type == "commit"; }
    bool local_branch() const { return full.starts_with("refs/heads/"); }
    bool remote_branch() const { return full.starts_with("refs/remotes/"); }
    bool tag() const { return full.starts_with("refs/tags/"); }
};

enum class merged_filter
{
    all,
    merged,    // reachable from HEAD
    unmerged,  // not
};

// The refs in full-name order, or an error. Annotated tags take their
// tagger as author and committer; local branches their description.
struct ref_list_result
{
    std::vector<ref_entry> refs;
    std::string error;
};
ref_list_result list_refs(const std::string& workdir, merged_filter = merged_filter::all);

// A ref's name with refs/heads/ (else refs/) taken off: main,
// remotes/origin/main, tags/v1 [spec 10, StripRefName].
std::string strip_ref_name(const std::string& full);

// The folders the refs make: "refs", then each path but the last segment
// ("refs/heads", "refs/heads/feature"), in order.
std::vector<std::string> ref_folders(const std::vector<ref_entry>&);

// Whether `text` (fields joined by '\n') matches a filter in the log's
// simple syntax: space-separated terms all present, -term absent, "quoted
// phrases", a leading ! negates; case-insensitive [spec 1.9.2, 10.6].
bool ref_filter_matches(const std::string& filter, const std::string& text);

}  // namespace tortilla
