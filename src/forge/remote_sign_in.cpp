#include "remote_sign_in.h"

#include "i18n.h"
#include "util/json.h"

#include <rapport/sys.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

namespace tortilla
{
namespace
{

bool same_host(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

// What anyone may send: no Authorization.
rap::sys::http_options anonymous(std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    return {.headers = {{"User-Agent", "Tortilla"}, {"Accept", "application/json"}},
            .timeout = timeout,
            .max_bytes = std::size_t(256) << 10};
}

// A path as one URL segment: GitLab's group/project as group%2Fproject.
std::string escaped(std::string_view s)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : s)
    {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~')
            out += c;
        else
        {
            out += '%';
            out += digits[u >> 4];
            out += digits[u & 15];
        }
    }
    return out;
}

}  // namespace

std::optional<forge_kind> known_forge(std::string_view host)
{
    for (forge_kind k : {forge_kind::github, forge_kind::gitlab, forge_kind::gitea})
        if (same_host(host, default_host(k))) return k;
    return std::nullopt;
}

std::optional<forge_kind> probe_forge(std::string_view host)
{
    if (auto k = known_forge(host)) return k;
    static std::mutex lock;
    static std::map<std::string, std::optional<forge_kind>, std::less<>> asked;
    {
        std::lock_guard hold(lock);
        if (auto it = asked.find(host); it != asked.end()) return it->second;
    }
    constexpr auto timeout = std::chrono::seconds(4);
    std::optional<forge_kind> kind;
    // Gitea and Forgejo answer anyone: {"version":"1.22.0"}.
    const auto gitea = rap::sys::http_get(api_root(forge_kind::gitea, host) + "/version",
                                          anonymous(timeout));
    const auto gitea_json = json::parse(gitea.body);
    if (gitea.status == 200 && gitea_json && (*gitea_json)["version"])
        kind = forge_kind::gitea;
    else if (gitea.status != 0)
    {
        // GitLab answers anyone with a 401 in its own words, or (where it's
        // open) the version.
        const auto gitlab = rap::sys::http_get(api_root(forge_kind::gitlab, host) + "/version",
                                               anonymous(timeout));
        const auto gitlab_json = json::parse(gitlab.body);
        if (gitlab_json && ((gitlab.status == 200 && (*gitlab_json)["version"]) ||
                            (gitlab.status == 401 && (*gitlab_json)["message"])))
            kind = forge_kind::gitlab;
    }
    if (gitea.status != 0)  // no answer at all is asked again next time
    {
        std::lock_guard hold(lock);
        asked.emplace(std::string(host), kind);
    }
    return kind;
}

repo_seen look_for_repo(forge_kind k, const forge_repo& r, std::string_view token)
{
    const std::string url = k == forge_kind::gitlab
                                ? api_root(k, r.host) + "/projects/" + escaped(r.path())
                                : api_root(k, r.host) + "/repos/" + r.path();
    // The answer's body isn't needed, only whether there is one.
    const auto reply = rap::sys::http_get(
        url, token.empty() ? anonymous()
                           : rap::sys::http_options{.headers = api_headers(k, token),
                                                    .max_bytes = std::size_t(256) << 10});
    if (reply.status == 200) return repo_seen::shown;
    if (reply.status == 404) return repo_seen::hidden;
    return repo_seen::unknown;
}

remote_sign_in describe_remote(bool on_forge, const forge_account* account, repo_seen seen,
                               bool asking)
{
    using offer = remote_sign_in::offer;
    if (!on_forge) return {};
    if (account)
    {
        if (seen == repo_seen::hidden)
            return {.summary = account->login,
                    .text = rap::i18n::format(_("{0} can't see this repository."), account->login),
                    .mark = check_state::failure,
                    .sign_in = offer::link};
        return {.summary = account->login,
                .text = rap::i18n::format(_("Signed in as {0}."), account->login),
                .mark = check_state::success};
    }
    switch (seen)
    {
    case repo_seen::hidden:
        return {.summary = _("Private"),
                .text = _("Private: sign in to see its pull requests and builds."),
                .sign_in = offer::button};
    case repo_seen::shown:
        return {.summary = _("Public"),
                .text = _("Public: sign in to merge and comment."),
                .sign_in = offer::link};
    case repo_seen::unknown: break;
    }
    return {.summary = _("Not signed in"),
            .text = asking ? _("Not signed in. Asking the forge whether the repository is public...")
                           : _("Not signed in: sign in to see its pull requests and builds."),
            .sign_in = offer::button};
}

}  // namespace tortilla
