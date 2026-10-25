#include "reflog.h"

#include "ref_list.h"
#include "repo.h"

#include <git2.h>

#include <algorithm>

namespace tortilla
{
namespace
{

// Where a ref sorts among the others: HEAD, local branches, remote
// branches, the stash, then the rest.
int group_of(const std::string& ref)
{
    if (ref == "HEAD") return 0;
    if (ref.starts_with("refs/heads/")) return 1;
    if (ref.starts_with("refs/remotes/")) return 2;
    if (ref == "refs/stash") return 3;
    return 4;
}

std::string hex_of(const git_oid* id)
{
    if (!id || git_oid_is_zero(id)) return {};
    char buf[GIT_OID_HEXSZ + 1];
    git_oid_tostr(buf, sizeof buf, id);
    return buf;
}

}  // namespace

std::vector<std::string> reflog_refs(const repo& r)
{
    std::vector<std::string> out;
    if (git_reference_has_log(r.raw(), "HEAD") == 1) out.emplace_back("HEAD");
    std::pair<git_repository*, std::vector<std::string>*> payload{r.raw(), &out};
    git_reference_foreach_name(
        r.raw(),
        [](const char* name, void* p)
        {
            auto* found = static_cast<std::pair<git_repository*, std::vector<std::string>*>*>(p);
            if (git_reference_has_log(found->first, name) == 1) found->second->emplace_back(name);
            return 0;
        },
        &payload);
    std::ranges::stable_sort(out,
                             [](const std::string& a, const std::string& b)
                             {
                                 const int ga = group_of(a), gb = group_of(b);
                                 return ga != gb ? ga < gb : a < b;
                             });
    return out;
}

std::expected<std::vector<reflog_entry>, std::string> read_reflog(const repo& r,
                                                                  const std::string& ref)
{
    git_reflog* log = nullptr;
    if (git_reflog_read(&log, r.raw(), ref.c_str()) != 0) return std::unexpected(last_git_error());
    const bool stash = ref == "refs/stash";
    std::vector<reflog_entry> out;
    const size_t n = git_reflog_entrycount(log);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const git_reflog_entry* e = git_reflog_entry_byindex(log, i);
        reflog_entry entry;
        entry.selector = (stash ? std::string("stash") : ref) + "@{" + std::to_string(i) + "}";
        entry.id = hex_of(git_reflog_entry_id_new(e));
        entry.old_id = hex_of(git_reflog_entry_id_old(e));
        const char* message = git_reflog_entry_message(e);
        std::tie(entry.action, entry.message) = split_reflog_message(message ? message : "", stash);
        if (const git_signature* who = git_reflog_entry_committer(e))
        {
            entry.committer = who->name ? who->name : "";
            entry.time = who->when.time;
        }
        out.push_back(std::move(entry));
    }
    git_reflog_free(log);
    return out;
}

std::pair<std::string, std::string> split_reflog_message(std::string_view message, bool stash)
{
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.remove_suffix(1);
    const size_t colon = stash ? std::string_view::npos : message.find(": ");
    if (colon == std::string_view::npos) return {std::string(), std::string(message)};
    return {std::string(message.substr(0, colon)), std::string(message.substr(colon + 2))};
}

std::string short_selector(const std::string& ref, size_t index)
{
    const std::string name = ref == "refs/stash" ? std::string("stash") : strip_ref_name(ref);
    return name + "@{" + std::to_string(index) + "}";
}

}  // namespace tortilla
