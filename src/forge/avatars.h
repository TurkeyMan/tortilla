// Authors' pictures, for the log's details pane [TortoiseGit: Log > View >
// Gravatar, spec 1.9.5]. Always on in Tortilla.
//
// Asked on a thread of its own, at the size asked for, in turn until one has
// a picture: a GitHub noreply address's own (no API call), Gravatar's (by
// the SHA-256 of the trimmed, lowercased email), then the forges hosting the
// repository's remotes, as the accounts signed in to them. The address goes
// to Gravatar and to those forges only. Kept in memory and in Tortilla's
// cache folder by that hash (a picture for a week, "has none" for a day,
// once every way has said so), so stepping through a history doesn't fetch
// again.
#pragma once

#include "forge/pr_watch.h"

#include <rapport/rapport.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tortilla
{

class avatars
{
public:
    // The picture, or null when the address has none (or it can't be had:
    // offline, say).
    using result = std::function<void(std::shared_ptr<rap::image>)>;

    // Whose picture: a commit's author, and the repository it's in, whose
    // forge remotes (pr_watch's) may know them.
    struct author
    {
        std::string email;
        std::string commit;  // its id
        std::string workdir;
        std::vector<forge_remote> remotes;
    };

    explicit avatars(rap::ui&);
    ~avatars();
    avatars(const avatars&) = delete;
    avatars& operator=(const avatars&) = delete;

    // The author's picture, `px` device pixels square, to `done` on the UI
    // thread: at once when it's known, else when fetched. Only the newest
    // request is answered; asking again drops the one before.
    void request(author, int px, result done);

private:
    struct job
    {
        std::string key, hash;
        int px = 0;
        unsigned id = 0;
        author who;
    };
    void run(std::stop_token);
    // The picture by the ways after the cache: nullopt when there's none,
    // `failed` set when a way couldn't say.
    std::optional<std::string> fetch(const job&, bool& failed);
    void finished(const job&, std::optional<std::string> bytes);

    rap::ui& ui_;
    // Expires with this, for what the thread posts to the UI thread.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::map<std::string, std::shared_ptr<rap::image>> cache_;  // UI thread; null: none
    result done_;                                               // the newest request's
    unsigned newest_ = 0;

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<job> pending_;
    std::atomic<bool> cancelled_{false};  // the fetch in progress is no longer wanted
    // The thread's: accounts' tokens, "host login" -> token, or none when
    // there's none to be had (not asked again: gh may be run for it).
    std::map<std::string, std::optional<std::string>> tokens_;
    std::jthread thread_;  // last: started once the rest exists
};

}  // namespace tortilla
