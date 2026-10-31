#include "badge_cache.h"

#include <rapport/sys.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tortilla
{

namespace fs = std::filesystem;
using clock = std::chrono::steady_clock;

namespace
{

// A change is loaded after things have been quiet this long (on top of the
// watch's own settling), so a checkout or build is one load, not hundreds.
constexpr auto k_quiet = std::chrono::milliseconds(500);
// A repository nobody has asked about for this long is let go, watch and all.
constexpr auto k_forget = std::chrono::minutes(10);
// How long where a folder's repository is (or that it has none) is believed.
constexpr auto k_root_memo = std::chrono::seconds(30);
// Paths remembered per repository, to say which changed: beyond this, a
// load reports none (the file manager redraws what it shows soon enough).
constexpr size_t k_max_asked = 100000;

struct repo_entry
{
    fs::path root;
    std::shared_ptr<const repo_badges> badges;  // null until first loaded
    std::unordered_set<std::string> asked;      // repository-relative
    std::unique_ptr<rap::sys::directory_watch> watch;
    clock::time_point used;
};

std::string key(const fs::path& p)
{
    return rap::sys::path_to_utf8(p);
}

// `path` inside `root`, '/'-separated as git has it; "" for root itself.
std::string repo_relative(const fs::path& path, const fs::path& root)
{
    std::string rel = rap::sys::path_to_utf8(path.lexically_relative(root));
    std::ranges::replace(rel, '\\', '/');
    return rel == "." ? std::string() : rel;
}

// Under .git, what changes without changing any file's status: objects,
// logs, locks, fetch bookkeeping.
bool ignorable(const fs::path& root, const fs::path& changed)
{
    const std::string rel = repo_relative(changed, root);
    if (!rel.starts_with(".git/")) return false;
    const std::string_view in = std::string_view(rel).substr(5);
    return in.starts_with("objects/") || in.starts_with("logs/") || in.ends_with(".lock") ||
           in == "FETCH_HEAD" || in == "ORIG_HEAD" || in == "gitk.cache";
}

}  // namespace

struct badge_cache::impl
{
    std::function<void(const std::vector<fs::path>&)> changed;
    std::mutex m;
    std::condition_variable wake;
    std::unordered_map<std::string, std::shared_ptr<repo_entry>> repos;  // by root
    struct root_memo
    {
        std::optional<fs::path> root;
        clock::time_point at;
    };
    std::unordered_map<std::string, root_memo> roots;  // by folder
    std::map<std::string, clock::time_point> due;      // roots to load, and when
    std::atomic<clock::rep> last_ask{clock::now().time_since_epoch().count()};
    bool stopping = false;
    std::thread worker;

    // The working tree holding `folder` (itself, if it has a .git), or
    // nullopt; inside a .git folder there's none.
    std::optional<fs::path> root_of(const fs::path& folder)
    {
        const std::string k = key(folder);
        const auto now = clock::now();
        if (auto it = roots.find(k); it != roots.end() && now - it->second.at < k_root_memo)
            return it->second.root;
        std::optional<fs::path> found;
        std::error_code ec;
        for (fs::path d = folder; !d.empty(); d = d.parent_path())
        {
            if (d.filename() == ".git") break;
            if (fs::exists(d / ".git", ec))
            {
                found = d;
                break;
            }
            if (d == d.parent_path()) break;
        }
        if (roots.size() > 50000) roots.clear();
        roots[k] = {found, now};
        return found;
    }

    void schedule(const std::string& root, clock::time_point when)
    {
        auto [it, added] = due.emplace(root, when);
        if (!added) it->second = std::max(it->second, when);
        wake.notify_one();
    }

    void watch(repo_entry& e)
    {
        const std::string root = key(e.root);
        auto w = rap::sys::directory_watch::start(
            e.root, {.recursive = true, .settle = std::chrono::milliseconds(300)},
            [this, root, at = e.root](rap::sys::watch_batch batch)
            {
                bool matters = batch.overflow;
                for (const auto& ev : batch.events)
                    matters = matters || !ignorable(at, ev.path) ||
                              (!ev.old_path.empty() && !ignorable(at, ev.old_path));
                if (!matters) return;
                std::lock_guard lock(m);
                if (repos.contains(root)) schedule(root, clock::now() + k_quiet);
            });
        if (w) e.watch = std::move(*w);
    }

    badge ask(const fs::path& path)
    {
        last_ask = clock::now().time_since_epoch().count();
        std::error_code ec;
        const bool dir = fs::is_directory(path, ec);
        std::lock_guard lock(m);
        const auto root = root_of(dir ? path : path.parent_path());
        if (!root) return badge::none;
        const std::string k = key(*root);
        auto& e = repos[k];
        if (!e)
        {
            e = std::make_shared<repo_entry>();
            e->root = *root;
            watch(*e);
            schedule(k, clock::now());
        }
        e->used = clock::now();
        const std::string rel = repo_relative(path, *root);
        if (e->asked.size() < k_max_asked) e->asked.emplace(rel);
        return e->badges ? e->badges->of(rel) : badge::none;
    }

    void run()
    {
        std::unique_lock lock(m);
        while (!stopping)
        {
            // Let go of repositories nobody asks about: their watches end
            // outside the lock, which a watch's callback may be waiting for.
            const auto now = clock::now();
            std::vector<std::shared_ptr<repo_entry>> gone;
            std::erase_if(repos,
                          [&](const auto& r)
                          {
                              if (now - r.second->used <= k_forget) return false;
                              gone.push_back(r.second);
                              return true;
                          });
            if (!gone.empty())
            {
                lock.unlock();
                gone.clear();
                lock.lock();
                continue;
            }
            if (due.empty())
            {
                wake.wait_for(lock, std::chrono::minutes(1));
                continue;
            }
            auto next = std::ranges::min_element(due, {}, [](const auto& d) { return d.second; });
            if (next->second > now)
            {
                wake.wait_until(lock, next->second);
                continue;
            }
            const std::string root = next->first;
            due.erase(next);
            auto entry = repos.contains(root) ? repos[root] : nullptr;
            if (!entry) continue;

            lock.unlock();
            auto loaded = load_badges(rap::sys::path_to_utf8(entry->root));
            lock.lock();
            if (!loaded) continue;  // not a working tree after all, or git failed
            auto fresh = std::make_shared<const repo_badges>(std::move(*loaded));
            std::vector<fs::path> differ;
            for (const std::string& rel : entry->asked)
                if ((entry->badges ? entry->badges->of(rel) : badge::none) != fresh->of(rel))
                    differ.push_back(rel.empty() ? entry->root
                                                 : entry->root / rap::sys::path_from_utf8(rel));
            entry->badges = std::move(fresh);
            if (!differ.empty() && changed)
            {
                lock.unlock();
                changed(differ);
                lock.lock();
            }
        }
    }
};

badge_cache::badge_cache(std::function<void(const std::vector<fs::path>&)> changed)
    : impl_(std::make_unique<impl>())
{
    impl_->changed = std::move(changed);
    impl_->worker = std::thread([i = impl_.get()] { i->run(); });
}

badge_cache::~badge_cache()
{
    {
        std::lock_guard lock(impl_->m);
        impl_->stopping = true;
    }
    impl_->wake.notify_one();
    impl_->worker.join();
    // The watches end outside the lock, their callbacks finding no repositories.
    decltype(impl_->repos) repos;
    {
        std::lock_guard lock(impl_->m);
        repos.swap(impl_->repos);
    }
}

badge badge_cache::ask(const fs::path& p)
{
    return impl_->ask(p);
}

std::chrono::steady_clock::duration badge_cache::idle() const
{
    return clock::now() - clock::time_point(clock::duration(impl_->last_ask.load()));
}

}  // namespace tortilla
