// Authors' pictures from Gravatar, for the log's details pane [TortoiseGit:
// Log > View > Gravatar, spec 1.9.5]. Always on in Tortilla.
//
// Fetched on a thread of its own by the SHA-256 of the trimmed, lowercased
// email, at the size asked for. Kept in memory and in Tortilla's cache folder
// (a picture for a week, "has none" for a day), so stepping through a
// history doesn't fetch again.
#pragma once

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

namespace tortilla
{

class gravatar
{
public:
    // The picture, or null when the address has none (or it can't be had:
    // offline, say).
    using result = std::function<void(std::shared_ptr<rap::image>)>;

    explicit gravatar(rap::ui&);
    ~gravatar();
    gravatar(const gravatar&) = delete;
    gravatar& operator=(const gravatar&) = delete;

    // `email`'s picture, `px` device pixels square, to `done` on the UI
    // thread: at once when it's known, else when fetched. Only the newest
    // request is answered; asking again drops the one before.
    void request(const std::string& email, int px, result done);

private:
    struct job
    {
        std::string key, hash;
        int px = 0;
        unsigned id = 0;
    };
    void run(std::stop_token);
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
    std::jthread thread_;                 // last: started once the rest exists
};

}  // namespace tortilla
